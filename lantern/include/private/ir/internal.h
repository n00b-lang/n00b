#ifndef IR_INTERNAL_H
#define IR_INTERNAL_H

#include <stdalign.h>
#include <stdarg.h>
#include "ir/ir.h"
#include "ir/layout.h"
#include "lib/arena.h"
#include "lib/buffer.h"
#include "parse/tree.h"
#include "parse/symtab.h"
#include "parse/annot_walk.h"

typedef struct ir_cleanup_t {
    void (*fn)(void *);
    void                *arg;
    struct ir_cleanup_t *next;
} ir_cleanup_t;

struct ir_ctx_t {
    core_arena_t  arena;
    th_ctx       *types;
    ir_cleanup_t *cleanups;
};

void ir_ctx_on_free(ir_ctx_t *ctx, void (*fn)(void *), void *arg);

typedef struct ir_descentry_t ir_descentry_t;

typedef struct {
    uint64_t        hash;
    th_type        *type;
    th_type        *def;
    ir_descentry_t *descs;
    size_t          n_descs;
    size_t          cap_descs;
} ir_typeslot_t;

struct ir_module_t {
    ir_ctx_t           *ctx;
    const char         *name;
    const char         *target;
    const ir_profile_t *profile;
    ir_props_t          props;
    ir_annots_t         annots;
    ir_span_t           span;

    parse_tree_t   *tree;
    parse_grammar_t      *tree_grammar;
    parse_symtab_t       *symtab;

    ir_typeslot_t *types;
    size_t         n_types;
    size_t         cap_types;
    void          *type_index;
    struct ir_rules_t *rules;

    uint64_t *scalar_hashes;
    th_type **scalar_types;
    bool      scalars_ready;

    uint64_t *ids;
    size_t    n_ids;
    size_t    cap_ids;
    void     *id_index;

    const char **files;
    size_t       n_files;
    size_t       cap_files;

    ir_global_t **globals;
    size_t        n_globals;
    size_t        cap_globals;
    void         *global_index;

    ir_func_t **funcs;
    size_t      n_funcs;
    size_t      cap_funcs;
    void        *func_index;
    void        *file_index;
    void        *ext_op_index;
};

struct ir_descentry_t {
    const char     *target;
    ir_descriptor_t d;
};

ir_typeslot_t *ir_type_slot(ir_module_t *m, th_type *t);
void ir_profile_install_builtins(ir_ctx_t *ctx, const ir_profile_t *p);
ir_typeslot_t *ir_type_slot_at(ir_module_t *m, size_t i);

void *ir_grow(ir_ctx_t *ctx, void *p, size_t *cap, size_t need, size_t elem);

void  ir_index_add(ir_ctx_t *ctx, void **slot, const char *name, void *node);
void *ir_index_find(void *slot, const char *name);

bool ir_local_read_earlier(const ir_instr_t *in, uint32_t o, ir_local_t *l);

typedef struct ir_rd_t   ir_rd_t;
typedef struct ir_doms_t ir_doms_t;

ir_rd_t    *ir_rd_of(ir_func_t *f, ir_doms_t *d);
uint32_t    ir_rd_count(ir_rd_t *r, ir_instr_t *use, ir_local_t *l);
ir_instr_t *ir_rd_at(ir_rd_t *r, ir_instr_t *use, ir_local_t *l, uint32_t i);
bool        ir_rd_maybe_unset(ir_rd_t *r, ir_instr_t *use, ir_local_t *l);
bool        ir_rd_reaches_a_use(ir_rd_t *r, ir_instr_t *def);

ir_rd_t    *ir_ssa_of(ir_func_t *f, ir_doms_t *d);
uint32_t    ir_rd_tok_count(const ir_rd_t *r);
uint32_t    ir_rd_phi_count(const ir_rd_t *r);
uint32_t    ir_rd_phis_in(const ir_rd_t *r, ir_block_t *b);
uint32_t    ir_rd_phi_at(const ir_rd_t *r, ir_block_t *b, uint32_t i);
ir_block_t *ir_rd_phi_block(const ir_rd_t *r, uint32_t p);
ir_local_t *ir_rd_phi_local(const ir_rd_t *r, uint32_t p);
uint32_t    ir_rd_phi_tok(const ir_rd_t *r, uint32_t p);
uint32_t    ir_rd_arm_count(const ir_rd_t *r, uint32_t p);
uint32_t    ir_rd_arm_tok(const ir_rd_t *r, uint32_t p, uint32_t i);
ir_block_t *ir_rd_arm_block(const ir_rd_t *r, uint32_t p, uint32_t i);
uint32_t    ir_rd_defs_in(const ir_rd_t *r, ir_block_t *b, uint32_t *first);
ir_instr_t *ir_rd_def_instr(const ir_rd_t *r, uint32_t d);
ir_local_t *ir_rd_def_local(const ir_rd_t *r, uint32_t d);
uint32_t    ir_rd_def_tok(const ir_rd_t *r, uint32_t d);
uint32_t    ir_rd_use_tok(ir_rd_t *r, ir_instr_t *use, ir_local_t *l);

void  ir_quote(core_buffer_t *b, const char *s);
void  ir_quote_n(core_buffer_t *b, const char *s, size_t length);
void  ir_lit_into(core_buffer_t *b, ir_lit_t v);
char *ir_lit_text(ir_lit_t v);

#endif
