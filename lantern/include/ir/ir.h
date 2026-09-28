#ifndef IR_H
#define IR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "th/typehash.h"

#define IR_VAR_ARITY UINT32_MAX

typedef enum : uint8_t {
    IR_MOD_OVERFLOW = 0x01,
    IR_MOD_DIVZERO  = 0x02,
    IR_MOD_SHIFT    = 0x04,
    IR_MOD_CMPORDER = 0x08,
    IR_MOD_ACCESS   = 0x10,
    IR_MOD_FIT      = 0x20,
    IR_MOD_ROUND    = 0x40,
} ir_modgroup_t;

typedef enum : uint8_t {
    IR_OPF_HASRESULT  = 0x01,
    IR_OPF_SIDEEFFECT = 0x02,
    IR_OPF_TERMINATOR = 0x04,
    IR_OPF_COMPTIME   = 0x08,
} ir_opflag_t;

typedef enum : uint8_t {
    IR_RES_NONE,
    IR_RES_ANNOTATED,
    IR_RES_OPERAND0,
    IR_RES_OPERAND1,
    IR_RES_BOOL,
    IR_RES_U32,
    IR_RES_U64,
    IR_RES_SINT,
    IR_RES_SINT32,
    IR_RES_STR,
    IR_RES_POINTEE,
    IR_RES_PTR_TO_ARG0,
    IR_RES_MEMBER,
    IR_RES_MEMBER_PTR,
    IR_RES_ELEM_PTR,
    IR_RES_VARIANT_ARM,
    IR_RES_CALLEE_RET,
} ir_resrule_t;

typedef enum : uint8_t {
    IR_OPD_VALUE,
    IR_OPD_SPREAD, /* Call-only expansion of a bound rest parameter. */
    IR_OPD_TYPE,
    IR_OPD_NAME,
    IR_OPD_BLOCK,
    IR_OPD_DEFAULT_BLOCK,
    IR_OPD_CONST,
    IR_OPD_PHI_ARM,
    IR_OPD_SWITCH_ARM,
    IR_OPD_NAMED_ARG,
    IR_OPD_ARG,
} ir_opdkind_t;

#define IR_OP(id, spelling, lo, hi, res, pat, mods, flags) IR_OP_##id,

typedef enum : uint16_t {
#include "ir_ops.def"
    IR_OP_COUNT,
} ir_op_t;

#undef IR_OP

typedef struct {
    const char  *spelling;
    uint32_t     min_operands;
    uint32_t     max_operands;
    ir_resrule_t result;
    const char  *pattern;
    uint8_t      mods;
    uint8_t      flags;
} ir_opinfo_t;

const ir_opinfo_t *ir_op_info(ir_op_t op);
bool               ir_op_lookup(const char *spelling, ir_op_t *out);
const char        *ir_op_spelling(ir_op_t op);
bool               ir_op_has_result(ir_op_t op);
bool               ir_op_is_terminator(ir_op_t op);
ir_opdkind_t       ir_op_operand_kind(ir_op_t op, uint32_t i);
const char        *ir_opdkind_name(ir_opdkind_t k);
bool               ir_op_accepts_operands(ir_op_t op, uint32_t n);
bool               ir_op_accepts_mod(ir_op_t op, const char *name);
bool               ir_op_is_extension(ir_op_t op);
bool               ir_opinfo_has_result(const ir_opinfo_t *i);
bool               ir_opinfo_is_terminator(const ir_opinfo_t *i);
ir_opdkind_t       ir_opinfo_operand_kind(const ir_opinfo_t *i, uint32_t k);
bool               ir_opinfo_accepts_operands(const ir_opinfo_t *i,
                                              uint32_t n);
bool               ir_opinfo_accepts_mod(const ir_opinfo_t *i,
                                         const char *name);
bool               ir_op_spelling_is_namespaced(const char *spelling,
                                                const char *profile);
bool               ir_mod_exists(const char *name);

typedef enum : uint16_t {
    IR_PROP_PARSED       = 0x0001,
    IR_PROP_TYPED        = 0x0002,
    IR_PROP_SSA          = 0x0004,
    IR_PROP_STRUCTURED   = 0x0008,
    IR_PROP_LAID_OUT     = 0x0010,
    IR_PROP_NO_CRIT_EDGE = 0x0020,
    IR_PROP_VERIFIED     = 0x0040,

    IR_PROP_TREE         = 0x0080,
    IR_PROP_SCOPED       = 0x0100,
    IR_PROP_SYMBOLS      = 0x0200,
    IR_PROP_CFLOW        = 0x0400,
    IR_PROP_TREE_TYPED   = 0x0800,
} ir_props_t;

const char *ir_prop_name(ir_props_t one);

typedef enum : uint8_t {
    IR_CLASS_OPAQUE,
    IR_CLASS_VOID,
    IR_CLASS_BOOL,
    IR_CLASS_SINT,
    IR_CLASS_UINT,
    IR_CLASS_FLOAT,
    IR_CLASS_PTR,
    IR_CLASS_AGGREGATE,
} ir_class_t;

typedef struct {
    ir_class_t class;
    uint32_t   width;
} ir_machine_t;

typedef struct ir_ctx_t     ir_ctx_t;
typedef struct ir_module_t  ir_module_t;
typedef struct ir_func_t    ir_func_t;
typedef struct ir_block_t   ir_block_t;
typedef struct ir_instr_t   ir_instr_t;
typedef struct ir_value_t   ir_value_t;
typedef struct ir_local_t   ir_local_t;
typedef struct ir_global_t  ir_global_t;
typedef struct ir_profile_t ir_profile_t;
typedef struct ir_region_t  ir_region_t;
struct ir_rule_t;

typedef struct {
    int32_t  file;
    uint32_t start_byte, end_byte;
    uint32_t start_line, start_col;
    uint32_t end_line, end_col;
    bool     implied;
} ir_span_t;

#define IR_SPAN_UNKNOWN ((ir_span_t){.file = -1})

typedef enum : uint8_t {
    IR_ORIGIN_SOURCE,
    IR_ORIGIN_INLINED_FROM,
    IR_ORIGIN_EXPANDED_FROM,
    IR_ORIGIN_LOWERED_FROM,
    IR_ORIGIN_GENERATED_BY,
} ir_origin_kind_t;

typedef struct ir_origin_t ir_origin_t;

struct ir_origin_t {
    ir_origin_kind_t kind;
    ir_span_t        span;
    const char      *agent;
    ir_origin_t     *parent;
};

typedef __int128          ir_int_t;
typedef unsigned __int128 ir_uint_t;

typedef struct {
    ir_uint_t mag;
    bool      neg;
    bool      hex;
} ir_lit_t;

typedef enum : uint8_t {
    IR_ANNOT_BOOL,
    IR_ANNOT_INT,
    IR_ANNOT_STR,
    IR_ANNOT_TYPE,
    IR_ANNOT_VALUE,
} ir_annot_kind_t;

typedef struct {
    const char     *key;
    ir_annot_kind_t kind;
    union {
        bool        b;
        ir_lit_t    i;
        const char *s;
        th_type    *t;
        ir_value_t *v;
    };
    ir_props_t derived_from;
} ir_annot_t;

typedef struct {
    ir_annot_t *a;
    uint32_t    n;
    uint32_t    cap;
    void       *index;
} ir_annots_t;

bool        ir_annot_set(ir_ctx_t *ctx, ir_annots_t *m, ir_annot_t a);
ir_annot_t *ir_annot_get(ir_annots_t *m, const char *key);
uint32_t    ir_annot_count(const ir_annots_t *m);
ir_annot_t *ir_annot_at(ir_annots_t *m, uint32_t i);
void        ir_annot_invalidate(ir_annots_t *m, ir_props_t lost);
bool        ir_annot_key_ok(const char *key, const char *profile);

typedef enum : uint8_t {
    IR_VAL_PARAM,
    IR_VAL_INSTR,
    IR_VAL_LOCAL,
    IR_VAL_GLOBAL,
    IR_VAL_FUNC,
} ir_valkind_t;

struct ir_value_t {
    ir_valkind_t kind;
    const char  *name;
    th_type     *type;
    ir_instr_t  *def;
    ir_local_t  *local;
    ir_func_t   *owner;
    uint32_t     index;
    bool         is_keyword;
    bool         is_rest;
};

typedef enum : uint8_t {
    IR_GLOBAL_INIT_INT,
    IR_GLOBAL_INIT_FLOAT,
    IR_GLOBAL_INIT_SYMBOL,
} ir_global_init_kind_t;

typedef struct {
    const char *member;
    ir_global_init_kind_t kind;
    union {
        ir_lit_t integer;
        double floating;
        ir_value_t *symbol;
    };
} ir_global_init_value_t;

typedef enum : uint8_t {
    IR_DEST_NONE,
    IR_DEST_VALUE,
    IR_DEST_LOCAL,
} ir_destkind_t;

typedef struct {
    ir_opdkind_t kind;
    union {
        ir_value_t  *value;
        th_type     *type;
        const char  *name;
        ir_block_t  *block;
        struct {
            ir_block_t *block;
            ir_value_t *value;
        } phi;
        struct {
            ir_lit_t    value;
            ir_block_t *block;
        } arm;
        struct {
            const char *name;
            ir_value_t *value;
        } named;
        struct {
            ir_lit_t    i;
            double      f;
            const char *text;
            bool        is_float;
        } lit;
    };
} ir_operand_t;

struct ir_instr_t {
    ir_op_t       op;
    ir_destkind_t dest_kind;
    ir_value_t   *dest;
    ir_local_t   *dest_local;
    ir_operand_t *operands;
    uint32_t      n_operands;
    uint32_t      cap_operands;
    const char  **mods;
    uint32_t      n_mods;
    uint32_t      cap_mods;
    ir_span_t     span;
    ir_origin_t  *origin;
    ir_annots_t   annots;
    ir_block_t   *block;
    uint32_t      index;
};

struct ir_block_t {
    const char   *name;
    ir_instr_t  **instrs;
    uint32_t      n_instrs;
    uint32_t      cap_instrs;
    ir_span_t     span;
    ir_origin_t  *origin;
    ir_annots_t   annots;
    ir_func_t    *owner;
    uint32_t      index;
};

typedef enum : uint8_t {
    IR_MUT_MUTABLE,
    IR_MUT_IMMUTABLE,
    IR_MUT_CONST,
} ir_mutability_t;

const char *ir_mutability_name(ir_mutability_t m);
bool        ir_mutability_from_name(const char *s, ir_mutability_t *out);

struct ir_local_t {
    const char     *name;
    th_type        *type;
    ir_mutability_t mutability;
    bool            has_init;
    ir_lit_t     init;
    double       init_f;
    const char  *init_text;
    bool         init_is_float;
    ir_span_t    span;
    ir_origin_t *origin;
    ir_annots_t  annots;
    uint32_t     index;
};

struct ir_global_t {
    const char  *name;
    th_type     *type;
    bool         is_static;
    bool         is_extern;
    ir_mutability_t mutability;
    bool         has_init;
    ir_lit_t     init;
    double       init_f;
    const char  *init_text;
    bool         init_is_float;
    bool         has_record_init;
    ir_global_init_value_t *record_init;
    size_t       n_record_init;
    ir_span_t    span;
    ir_origin_t *origin;
    ir_annots_t  annots;
};

typedef enum : uint8_t {
    IR_LINK_INTERNAL,
    IR_LINK_EXTERNAL,
    IR_LINK_EXTERN,
} ir_linkage_t;

struct ir_func_t {
    const char   *name;
    th_type      *type;
    ir_linkage_t  linkage;
    ir_value_t  **params;
    uint32_t      n_params;
    uint32_t      cap_params;
    ir_local_t  **locals;
    uint32_t      n_locals;
    uint32_t      cap_locals;
    ir_block_t  **blocks;
    uint32_t      n_blocks;
    uint32_t      cap_blocks;
    void         *block_index;
    void         *local_index;
    void         *value_index;
    ir_region_t  *regions;
    ir_span_t     span;
    ir_origin_t  *origin;
    ir_annots_t   annots;
    ir_props_t    props;
    ir_module_t  *owner;
};

ir_ctx_t *ir_ctx_new(void);
void      ir_ctx_free(ir_ctx_t *ctx);
size_t    ir_ctx_bytes(const ir_ctx_t *ctx);
th_ctx   *ir_ctx_types(ir_ctx_t *ctx);
void     *ir_alloc(ir_ctx_t *ctx, size_t n);
char     *ir_strdup(ir_ctx_t *ctx, const char *s);

ir_module_t *ir_module_new(ir_ctx_t *ctx, const char *name,
                           const ir_profile_t *profile, const char *target);
const char  *ir_module_name(const ir_module_t *m);
const char  *ir_module_target(const ir_module_t *m);
const ir_profile_t *ir_module_profile(const ir_module_t *m);
void                ir_module_set_profile(ir_module_t *m,
                                           const ir_profile_t *p);
ir_props_t   ir_module_props(const ir_module_t *m);
void         ir_module_establish(ir_module_t *m, ir_props_t p);
void         ir_module_invalidate(ir_module_t *m, ir_props_t p);
ir_annots_t *ir_module_annots(ir_module_t *m);
ir_span_t    ir_module_span(const ir_module_t *m);
void         ir_module_set_span(ir_module_t *m, ir_span_t s);
void         ir_module_set_target(ir_module_t *m, const char *target);

th_type *ir_type_intern(ir_module_t *m, th_type *t);
th_type *ir_type_parse(ir_module_t *m, const char *text);
/* Returns the return type for an infallible callable, or a module-local
 * tagged outcome type with `ok` and `error` arms for a callable with a
 * non-void error channel. */
th_type *ir_call_outcome_type(ir_module_t *m, th_type *callable);
uint64_t ir_type_id(ir_module_t *m, th_type *t);
bool     ir_type_eq(ir_module_t *m, th_type *a, th_type *b);
size_t   ir_type_count(const ir_module_t *m);
th_type *ir_type_at(ir_module_t *m, size_t i);
bool     ir_type_define(ir_module_t *m, th_type *named, th_type *def);
th_type *ir_type_definition(ir_module_t *m, th_type *named);

int32_t     ir_file_add(ir_module_t *m, const char *path);
const char *ir_file_path(const ir_module_t *m, int32_t id);
size_t      ir_file_count(const ir_module_t *m);

ir_span_t    ir_span_make(int32_t file, uint32_t sb, uint32_t eb,
                          uint32_t sl, uint32_t sc, uint32_t el, uint32_t ec);
bool         ir_span_known(ir_span_t s);
ir_origin_t *ir_origin_new(ir_ctx_t *ctx, ir_origin_kind_t kind,
                           ir_span_t span, const char *agent,
                           ir_origin_t *parent);
uint32_t     ir_origin_depth(const ir_origin_t *o);

ir_global_t *ir_global_add(ir_module_t *m, const char *name, th_type *type);
ir_value_t  *ir_global_address(ir_module_t *m, ir_global_t *global);
ir_value_t  *ir_func_address(ir_module_t *m, ir_func_t *function);
/* Set a constant record initializer by member name. Symbol values
 * must name a function/global whose address type matches the corresponding
 * indirection member. */
bool         ir_global_set_record_initializer(
    ir_module_t *m, ir_global_t *global,
    const ir_global_init_value_t *values, size_t count);
ir_global_t *ir_global_find(ir_module_t *m, const char *name);
size_t       ir_global_count(const ir_module_t *m);
ir_global_t *ir_global_at(ir_module_t *m, size_t i);

ir_func_t   *ir_func_add(ir_module_t *m, const char *name, th_type *type,
                         ir_linkage_t linkage);
ir_func_t   *ir_func_find(ir_module_t *m, const char *name);
size_t       ir_func_count(const ir_module_t *m);
ir_func_t   *ir_func_at(ir_module_t *m, size_t i);
ir_value_t  *ir_param_add(ir_func_t *f, const char *name, th_type *type);
ir_value_t  *ir_kwparam_add(ir_func_t *f, const char *name, th_type *type);
ir_value_t  *ir_restparam_add(ir_func_t *f, const char *name, th_type *type);
ir_local_t  *ir_local_add(ir_func_t *f, const char *name, th_type *type);
ir_local_t  *ir_local_find(ir_func_t *f, const char *name);
ir_value_t  *ir_value_find(ir_func_t *f, const char *name);
ir_block_t  *ir_block_add(ir_func_t *f, const char *name);
ir_block_t  *ir_block_find(ir_func_t *f, const char *name);
ir_block_t  *ir_block_entry(ir_func_t *f);
ir_instr_t  *ir_block_terminator(ir_block_t *b);

ir_instr_t *ir_instr_add(ir_block_t *b, ir_op_t op);
bool        ir_instr_set_dest(ir_instr_t *in, const char *name, th_type *type);
bool        ir_instr_set_dest_local(ir_instr_t *in, ir_local_t *l);
bool        ir_instr_operand_value(ir_instr_t *in, ir_value_t *v);
/* Append a call-only spread operand for a parameter added with
 * ir_restparam_add(). */
bool        ir_instr_operand_spread(ir_instr_t *in, ir_value_t *v);
bool        ir_instr_operand_local(ir_instr_t *in, ir_local_t *l);
bool        ir_instr_operand_named(ir_instr_t *in, const char *name,
                                   ir_value_t *v);
bool        ir_instr_operand_type(ir_instr_t *in, th_type *t);
bool        ir_instr_operand_name(ir_instr_t *in, const char *n);
bool        ir_instr_operand_block(ir_instr_t *in, ir_block_t *b);
bool        ir_instr_operand_default(ir_instr_t *in, ir_block_t *b);
bool        ir_instr_operand_int(ir_instr_t *in, ir_int_t v);
bool        ir_instr_operand_uint(ir_instr_t *in, ir_uint_t v);
bool        ir_instr_operand_int_lit(ir_instr_t *in, ir_lit_t v);
bool        ir_instr_operand_float(ir_instr_t *in, double v);
bool        ir_instr_operand_float_text(ir_instr_t *in, const char *text);
bool        ir_instr_operand_phi_arm(ir_instr_t *in, ir_block_t *b,
                                     ir_value_t *v);
bool        ir_instr_operand_switch_arm(ir_instr_t *in, ir_int_t v,
                                        ir_block_t *b);
bool        ir_instr_operand_switch_arm_lit(ir_instr_t *in, ir_lit_t v,
                                            ir_block_t *b);
bool        ir_lit_eq(ir_lit_t a, ir_lit_t b);
ir_lit_t    ir_lit_of(ir_int_t v);
ir_lit_t    ir_lit_hex(ir_uint_t v);
ir_int_t    ir_lit_signed(ir_lit_t v);
bool        ir_instr_add_mod(ir_instr_t *in, const char *mod);
bool        ir_instr_has_mod(const ir_instr_t *in, const char *mod);
ir_value_t *ir_operand_value(const ir_operand_t *od);
ir_local_t *ir_operand_local(const ir_operand_t *od);

struct ir_profile_t {
    const char *name;
    size_t      n_scalars;
    const struct ir_scalar_t *scalars;
    size_t      n_rules;
    const struct ir_rule_t *rules;
    size_t      n_ext_ops;
    const ir_opinfo_t *ext_ops;
};

typedef enum : uint8_t {
    IR_ROLE_NONE,
    IR_ROLE_CHAR,
} ir_role_t;

typedef struct ir_scalar_t {
    const char  *type_text;
    ir_class_t   class;
    uint32_t     width;
    ir_role_t    role;
} ir_scalar_t;

bool ir_module_op_lookup(ir_module_t *m, const char *spelling, ir_op_t *out);
const ir_opinfo_t *ir_module_op_info(ir_module_t *m, ir_op_t op);
const char        *ir_module_op_spelling(ir_module_t *m, ir_op_t op);

ir_machine_t ir_machine_of(ir_module_t *m, th_type *t);
bool         ir_profile_has(ir_module_t *m, th_type *t);
th_type     *ir_profile_scalar(ir_module_t *m, ir_class_t c, uint32_t width);
th_type     *ir_profile_role(ir_module_t *m, ir_role_t role);

typedef struct ir_diags_t ir_diags_t;

typedef bool (*ir_pass_fn)(ir_module_t *m, ir_diags_t *d, void *state);

typedef struct {
    const char *name;
    ir_props_t  requires;
    ir_props_t  establishes;
    ir_props_t  invalidates;
    ir_pass_fn  run;
} ir_pass_t;

typedef enum : uint8_t {
    IR_PASS_OK,
    IR_PASS_UNMET,
    IR_PASS_FAILED,
    IR_PASS_INVALID,
} ir_pass_result_t;

bool             ir_pass_declares(const ir_pass_t *p);

extern const ir_profile_t ir_profile_bce;
extern const ir_profile_t ir_profile_synth;

#endif
