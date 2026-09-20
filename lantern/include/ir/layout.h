#ifndef IR_LAYOUT_H
#define IR_LAYOUT_H

#include "ir/diag.h"
#include "ir/ir.h"

typedef enum : uint8_t {
    IR_LAYOUT_UNSET,
    IR_LAYOUT_PINNED,
    IR_LAYOUT_SYMBOLIC,
    IR_LAYOUT_RESOLVED,
} ir_layout_state_t;

typedef enum : uint8_t {
    IR_SYM_SIZEOF,
    IR_SYM_ALIGNOF,
    IR_SYM_OFFSETOF,
} ir_symkind_t;

typedef struct {
    ir_layout_state_t state;
    bool              authored;
    uint64_t          value;
    ir_symkind_t      sym;
    th_type          *of;
    const char       *member;
} ir_layout_val_t;

typedef struct {
    const char     *name;
    ir_layout_val_t bit_offset;
    ir_layout_val_t bit_width;
} ir_member_layout_t;

typedef struct {
    bool            fat;
    bool            authored;
    ir_layout_val_t words;
} ir_indirect_repr_t;

typedef struct {
    ir_layout_val_t tag_bit_offset;
    ir_layout_val_t tag_bit_width;
    bool            niche;
    bool            authored;
} ir_variant_repr_t;

typedef struct {
    th_type            *type;
    const char         *target;
    ir_layout_val_t     size;
    ir_layout_val_t     align;
    ir_layout_val_t     stride;
    ir_member_layout_t *members;
    uint32_t            n_members;
    void               *member_index;
    ir_indirect_repr_t  indirect;
    ir_variant_repr_t   variant;
    bool                done;
} ir_descriptor_t;

const char *ir_layout_state_name(ir_layout_state_t s);
const char *ir_symkind_name(ir_symkind_t k);
bool        ir_symkind_from_name(const char *s, ir_symkind_t *out);

ir_descriptor_t *ir_layout_of(ir_module_t *m, th_type *t, const char *target);
ir_descriptor_t *ir_layout_of_targetless(ir_module_t *m, th_type *t);
ir_descriptor_t *ir_layout_ensure(ir_module_t *m, th_type *t,
                                  const char *target);
ir_descriptor_t *ir_layout_ensure_targetless(ir_module_t *m, th_type *t);
size_t           ir_layout_count(ir_module_t *m, th_type *t);

bool ir_layout_pin_size(ir_module_t *m, th_type *t, uint64_t bytes);
bool ir_layout_pin_align(ir_module_t *m, th_type *t, uint64_t bytes);
bool ir_layout_pin_stride(ir_module_t *m, th_type *t, uint64_t bytes);
bool ir_layout_pin_member(ir_module_t *m, th_type *t, const char *field,
                          uint64_t bit_offset, uint64_t bit_width);
bool ir_layout_descriptor_pin_member(ir_module_t *m, ir_descriptor_t *d,
                                     const char *field, uint64_t bit_offset,
                                     uint64_t bit_width);
bool ir_layout_pin_variant(ir_module_t *m, th_type *t, ir_variant_repr_t r);
bool ir_layout_pin_indirect(ir_module_t *m, th_type *t, ir_indirect_repr_t r);

bool ir_layout_run(ir_module_t *m, ir_diags_t *d);
bool ir_layout_is_settled(const ir_layout_val_t *v);

extern const ir_pass_t ir_pass_layout;

#endif
