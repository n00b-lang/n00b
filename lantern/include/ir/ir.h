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
    IR_RES_CALL_OUTPUT,
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
    IR_OPD_OUT,
    IR_OPD_FRESH,
    IR_OPD_MISSING,
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
typedef struct ir_shape_t   ir_shape_t;
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
    ir_global_t *global;
    ir_func_t   *owner;
    uint32_t     index;
    bool         is_keyword;
    bool         is_rest;
    bool         is_optional;
    ir_shape_t  *shape;
    ir_annots_t  annots;
};

typedef enum : uint8_t {
    IR_GLOBAL_INIT_BOOL,
    IR_GLOBAL_INIT_INT,
    IR_GLOBAL_INIT_FLOAT,
    IR_GLOBAL_INIT_STRING,
    IR_GLOBAL_INIT_SYMBOL,
    IR_GLOBAL_INIT_PHASE_RESULT,
    IR_GLOBAL_INIT_SEQUENCE,
    IR_GLOBAL_INIT_TUPLE,
    IR_GLOBAL_INIT_RECORD,
    IR_GLOBAL_INIT_SET,
    IR_GLOBAL_INIT_DICTIONARY,
} ir_global_init_kind_t;

typedef struct ir_global_init_value ir_global_init_value_t;

struct ir_global_init_value {
    const char *member;
    ir_global_init_kind_t kind;
    union {
        bool boolean;
        ir_lit_t integer;
        double floating;
        struct {
            const char *bytes;
            size_t length;
        } string;
        ir_value_t *symbol;
        struct {
            const char *identifier;
        } phase_result;
        struct {
            const ir_global_init_value_t *items;
            size_t count;
        } aggregate;
    };
};

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
    ir_shape_t  *shape_constraint;
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

typedef enum : uint8_t {
    IR_PHASE_RUNTIME,
    IR_PHASE_COMPTIME,
} ir_execution_phase_t;

typedef enum : uint8_t {
    IR_PHASE_ACCESS_NONE = 0,
    IR_PHASE_ACCESS_COMPTIME = 1u << 0,
    IR_PHASE_ACCESS_RUNTIME = 1u << 1,
    IR_PHASE_ACCESS_BOTH = IR_PHASE_ACCESS_COMPTIME
                         | IR_PHASE_ACCESS_RUNTIME,
} ir_phase_access_t;

typedef enum : uint8_t {
    IR_INDIRECT_ACCESS_UNSPECIFIED,
    IR_INDIRECT_ACCESS_READ,
    IR_INDIRECT_ACCESS_WRITE,
    IR_INDIRECT_ACCESS_READ_WRITE,
} ir_indirect_access_t;

typedef enum : uint8_t {
    IR_STORAGE_DURATION_UNSPECIFIED,
    IR_STORAGE_DURATION_COMPTIME,
    IR_STORAGE_DURATION_AUTOMATIC,
    IR_STORAGE_DURATION_MODULE,
    IR_STORAGE_DURATION_THREAD,
    IR_STORAGE_DURATION_DYNAMIC,
} ir_storage_duration_t;

const char *ir_mutability_name(ir_mutability_t m);
bool        ir_mutability_from_name(const char *s, ir_mutability_t *out);
/* Functions default to runtime. Phase is retained as ordinary IR metadata. */
bool        ir_func_set_execution_phase(ir_ctx_t *ctx, ir_func_t *function,
                                        ir_execution_phase_t phase);
ir_execution_phase_t ir_func_execution_phase(const ir_func_t *function);
bool ir_func_set_initializer(ir_ctx_t *ctx, ir_func_t *function,
                             bool is_initializer);
bool ir_func_is_initializer(const ir_func_t *function);
/* Per-binding phase facts are ordinary metadata and survive IR text round
 * trips. Missing metadata preserves legacy unrestricted access. */
bool ir_value_set_phase_access(ir_ctx_t *ctx, ir_value_t *value,
                               ir_phase_access_t available,
                               ir_phase_access_t mutable_in,
                               ir_phase_access_t initializable_in);
bool ir_value_can_read(const ir_value_t *value, ir_execution_phase_t phase);
bool ir_value_can_write(const ir_value_t *value, ir_execution_phase_t phase);
bool ir_value_can_initialize(const ir_value_t *value,
                             ir_execution_phase_t phase);
/* Access through an indirect parameter is an effect contract, distinct from
 * the parameter's by-value IR type and from the caller's target identity. */
bool ir_value_set_indirect_access(ir_ctx_t *ctx, ir_value_t *parameter,
                                  ir_indirect_access_t access);
ir_indirect_access_t ir_value_indirect_access(const ir_value_t *parameter);
/* Marks a binding whose lexical lifetime is re-entered at a loop header. */
bool ir_value_set_binding_reentry(ir_value_t *value,
                                  ir_block_t *loop_header);
/* The existing global mutability is the runtime contract. */
bool        ir_global_set_comptime_mutability(ir_ctx_t *ctx,
                                              ir_global_t *global,
                                              ir_mutability_t mutability);
ir_mutability_t ir_global_comptime_mutability(const ir_global_t *global);
/* Phase access is independent of mutability and initialization eligibility.
 * Unannotated globals retain legacy availability in every phase. */
bool ir_global_set_phase_access(ir_ctx_t *ctx, ir_global_t *global,
                                ir_phase_access_t available,
                                ir_phase_access_t mutable_in,
                                ir_phase_access_t initializable_in);
bool ir_global_can_read(const ir_global_t *global,
                        ir_execution_phase_t phase);
bool ir_global_can_write(const ir_global_t *global,
                         ir_execution_phase_t phase);
bool ir_global_can_initialize(const ir_global_t *global,
                              ir_execution_phase_t phase);
bool ir_global_set_storage_class(ir_ctx_t *ctx, ir_global_t *global,
                                 const char *requested, bool required,
                                 const char *selected);
const char *ir_global_storage_class(const ir_global_t *global,
                                   bool *required,
                                   const char **requested);
bool ir_global_set_storage_duration(ir_ctx_t *ctx, ir_global_t *global,
                                    ir_storage_duration_t duration);
ir_storage_duration_t ir_global_storage_duration(
    const ir_global_t *global);

struct ir_local_t {
    const char     *name;
    th_type        *type;
    ir_value_t     *value;
    ir_mutability_t mutability;
    bool            address_taken;
    bool            has_init;
    ir_lit_t     init;
    double       init_f;
    const char  *init_text;
    bool         init_is_float;
    ir_span_t    span;
    ir_origin_t *origin;
    ir_shape_t  *shape;
    ir_annots_t  annots;
    uint32_t     index;
};

struct ir_global_t {
    ir_module_t *owner;
    ir_global_t *identity;
    const char  *name;
    th_type     *type;
    bool         is_static;
    bool         is_extern;
    ir_mutability_t mutability;
    bool         has_static_initializer;
    ir_global_init_value_t *static_initializer;
    ir_span_t    span;
    ir_origin_t *origin;
    ir_shape_t  *shape;
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
    bool         *optional_parameters;
    uint32_t      n_optional_parameters;
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
/* Returns the return type for an infallible callable, or a context-wide
 * tagged outcome type with `ok` and `error` arms for a callable with a
 * non-void error channel. */
th_type *ir_call_outcome_type(ir_module_t *m, th_type *callable);
size_t   ir_call_output_count(th_type *callable);
th_type *ir_call_output_type(th_type *callable, size_t output_index);
uint64_t ir_type_id(ir_module_t *m, th_type *t);
bool     ir_type_eq(ir_module_t *m, th_type *a, th_type *b);
size_t   ir_type_count(const ir_module_t *m);
th_type *ir_type_at(ir_module_t *m, size_t i);
bool     ir_type_define(ir_module_t *m, th_type *named, th_type *def);
th_type *ir_type_definition(ir_module_t *m, th_type *named);
ir_module_t *ir_type_definition_owner(ir_module_t *m, th_type *named);

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
/* Relate a module-local global declaration to its canonical cross-module
 * identity. The identity root must have the same name and type. */
bool ir_global_set_identity(ir_ctx_t *ctx, ir_global_t *global,
                            ir_global_t *identity);
ir_global_t *ir_global_identity(const ir_global_t *global);
ir_module_t *ir_global_owner(const ir_global_t *global);
/* Set a global's static initializer from length-delimited string bytes. */
bool ir_global_set_string_initializer(ir_module_t *m, ir_global_t *global,
                                     const char *bytes, size_t length);
/* Returns an address for a module-owned global. A foreign canonical global
 * may be used when this module declares a same-name, same-type extern. */
ir_value_t  *ir_global_address(ir_module_t *m, ir_global_t *global);
ir_value_t  *ir_func_address(ir_module_t *m, ir_func_t *function);
/* Set a constant record initializer by member name. Symbol values
 * must name a function/global whose address type matches the corresponding
 * indirection member. */
bool         ir_global_set_record_initializer(
    ir_module_t *m, ir_global_t *global,
    const ir_global_init_value_t *values, size_t count);
/* Set a recursively typed static initializer. Aggregate child nodes are
 * copied into the IR context; record children name their corresponding field. */
bool ir_global_set_static_initializer(
    ir_module_t *m, ir_global_t *global,
    const ir_global_init_value_t *value);
/* Replace a literal initializer with a typed result supplied by another
 * compilation phase. Repeating the same identifier is idempotent. */
bool ir_global_set_phase_result_initializer(
    ir_module_t *m, ir_global_t *global, const char *identifier);
bool ir_global_static_initializer_valid(
    ir_module_t *m, const ir_global_t *global);
ir_global_t *ir_global_find(ir_module_t *m, const char *name);
size_t       ir_global_count(const ir_module_t *m);
ir_global_t *ir_global_at(ir_module_t *m, size_t i);

ir_func_t   *ir_func_add(ir_module_t *m, const char *name, th_type *type,
                         ir_linkage_t linkage);
ir_func_t   *ir_func_find(ir_module_t *m, const char *name);
/* Preserve a universally quantified callable contract separately from the
 * function's IR parameter/result shape. Calls instantiate this contract per
 * use; the contract itself is never refined by a call. */
bool         ir_func_set_callable_contract(ir_func_t *function,
                                           th_type *contract);
th_type     *ir_func_callable_contract(const ir_func_t *function);
bool         ir_func_set_parameter_optional(ir_func_t *function,
                                            uint32_t index, bool optional);
bool         ir_func_parameter_is_optional(const ir_func_t *function,
                                           uint32_t index);
size_t       ir_func_count(const ir_module_t *m);
ir_func_t   *ir_func_at(ir_module_t *m, size_t i);
ir_value_t  *ir_param_add(ir_func_t *f, const char *name, th_type *type);
bool         ir_param_set_optional(ir_value_t *parameter, bool optional);
bool         ir_param_is_optional(const ir_value_t *parameter);
ir_value_t  *ir_kwparam_add(ir_func_t *f, const char *name, th_type *type);
ir_value_t  *ir_restparam_add(ir_func_t *f, const char *name, th_type *type);
ir_local_t  *ir_local_add(ir_func_t *f, const char *name, th_type *type);
ir_local_t  *ir_local_find(ir_func_t *f, const char *name);
ir_value_t  *ir_value_find(ir_func_t *f, const char *name);
ir_value_t  *ir_local_value(ir_func_t *f, ir_local_t *local);
ir_annots_t *ir_value_annots(ir_value_t *value);
typedef enum : uint8_t {
    IR_LIVENESS_UNKNOWN,
    IR_LIVENESS_DEAD,
    IR_LIVENESS_LIVE,
} ir_liveness_t;
typedef struct {
    ir_value_t *value;
    bool escapes;
} ir_alias_target_t;
typedef bool (*ir_store_target_visitor_fn)(void *context,
                                           ir_value_t *target);
/* Attach a complete or partial set of known storage aliases to a value that
 * may refer to external storage, including non-pointer views. The matching
 * reverse edges are added to each target. */
bool ir_value_set_alias_targets(ir_ctx_t *ctx, ir_value_t *value,
                                const ir_alias_target_t *targets,
                                size_t target_count, bool complete);
/* Add a newly discovered target to an alias set. Incremental additions make
 * the set partial until a later whole-analysis pass proves completeness. */
bool ir_value_add_alias_target(ir_ctx_t *ctx, ir_value_t *value,
                               ir_alias_target_t target);
/* Copy alias facts and reverse edges when a value-preserving projection
 * produces a new value, such as extracting a successful call result. */
bool ir_value_copy_alias_targets(ir_ctx_t *ctx, ir_value_t *destination,
                                 const ir_value_t *source);
/* Visits the known storage targets of an indirect store address. Returns
 * false if the target set is incomplete or the visitor stops traversal. */
bool ir_value_visit_store_targets(const ir_value_t *address,
                                  ir_store_target_visitor_fn visitor,
                                  void *context);
/* Visits symbolic storage origins for an indirect store address. Formal
 * parameters remain origins even when call-site alias facts are attached. */
bool ir_value_visit_store_origins(const ir_value_t *address,
                                  ir_store_target_visitor_fn visitor,
                                  void *context);
/* Returns LIVE if a direct operand use is reachable after `point`, DEAD only
 * when no reachable future use exists, and UNKNOWN for incomplete CFG facts.
 * PHI operands are conservatively treated as uses in their destination block. */
ir_liveness_t ir_value_liveness_after(const ir_value_t *value,
                                      const ir_instr_t *point);
/* Queries one CFG edge without merging the other successors of its source.
 * The edge must be present in the source block's terminator. */
ir_liveness_t ir_value_liveness_on_edge(const ir_value_t *value,
                                        const ir_block_t *from,
                                        const ir_block_t *to);
/* Extends the direct-use query over the value's complete annotated alias
 * component. Escaping or incomplete alias facts produce UNKNOWN. */
ir_liveness_t ir_value_alias_liveness_after(const ir_value_t *value,
                                             const ir_instr_t *point);
ir_liveness_t ir_value_alias_liveness_on_edge(const ir_value_t *value,
                                              const ir_block_t *from,
                                              const ir_block_t *to);
typedef struct {
    ir_func_t *function;
    ir_value_t *binding;
    ir_instr_t *point;
    ir_block_t *from;
    ir_block_t *to;
} ir_lifetime_endpoint_t;
typedef bool (*ir_lifetime_endpoint_visitor_fn)(
    void *context, ir_lifetime_endpoint_t endpoint);
/* Visits safe last-use endpoints: an instruction point has `point` set, and
 * a branch-specific endpoint has `from` and `to` set. Unknown or escaping
 * alias facts are omitted. Returns false for an incomplete CFG or a visitor
 * that stops traversal. This does not schedule cleanup or finalization. */
bool ir_value_visit_lifetime_endpoints(
    const ir_value_t *value, ir_lifetime_endpoint_visitor_fn visitor,
    void *context);
/* Checks alias/lifetime facts at a candidate point; does not schedule or
 * guarantee finalization or reclamation. */
bool         ir_value_can_finalize_at(const ir_value_t *value,
                                      uint64_t point);
/* True only when the value and its complete alias component have no
 * reachable uses after this CFG edge. This is an eligibility check, not a
 * storage-policy decision or a cleanup/finalizer scheduling operation. */
bool         ir_value_can_finalize_on_edge(const ir_value_t *value,
                                            const ir_block_t *from,
                                            const ir_block_t *to);
ir_block_t  *ir_block_add(ir_func_t *f, const char *name);
ir_block_t  *ir_block_find(ir_func_t *f, const char *name);
ir_block_t  *ir_block_entry(ir_func_t *f);
ir_instr_t  *ir_block_terminator(ir_block_t *b);

ir_instr_t *ir_instr_add(ir_block_t *b, ir_op_t op);
bool        ir_instr_set_dest(ir_instr_t *in, const char *name, th_type *type);
bool        ir_instr_set_dest_local(ir_instr_t *in, ir_local_t *l);
/* Storage classes are extensible names. A null class clears the request. */
bool        ir_instr_set_storage_class(ir_instr_t *allocation,
                                       const char *storage_class,
                                       bool required);
const char *ir_instr_storage_class(const ir_instr_t *allocation,
                                   bool *required);
bool ir_instr_set_binding_initialization(ir_instr_t *instruction,
                                         bool initializes);
bool ir_instr_is_binding_initialization(const ir_instr_t *instruction);
bool ir_instr_set_shape_constraint(ir_instr_t *instruction,
                                   ir_shape_t *shape);
typedef enum : uint8_t {
    IR_AGGREGATE_ELEMENTS,
    IR_AGGREGATE_PAIRS,
} ir_aggregate_form_t;
bool ir_instr_set_aggregate_form(ir_instr_t *instruction,
                                 ir_aggregate_form_t form);
bool ir_instr_aggregate_form(const ir_instr_t *instruction,
                             ir_aggregate_form_t *form);
bool ir_instr_set_const_bytes(ir_instr_t *instruction,
                              const uint8_t *bytes, size_t byte_len);
const char *ir_instr_const_bytes_hex(const ir_instr_t *instruction,
                                     size_t *byte_len);
/* Retain a generic call site's correlated, instantiated signature. A typed
 * generic call requires this so the IR does not re-infer from a frozen
 * universal declaration independently at each operand. */
bool ir_instr_set_call_signature(ir_instr_t *call, th_type *signature);
th_type *ir_instr_call_signature(const ir_instr_t *call);
bool ir_instr_set_phase_result(ir_ctx_t *ctx, ir_instr_t *instruction,
                               const char *identifier);
const char *ir_instr_phase_result(const ir_instr_t *instruction);
bool        ir_instr_operand_value(ir_instr_t *in, ir_value_t *v);
/* Append a call-only spread operand for a parameter added with
 * ir_restparam_add(). */
bool        ir_instr_operand_spread(ir_instr_t *in, ir_value_t *v);
bool        ir_instr_operand_local(ir_instr_t *in, ir_local_t *l);
bool        ir_instr_operand_named(ir_instr_t *in, const char *name,
                                   ir_value_t *v);
bool        ir_instr_operand_named_out(ir_instr_t *in, const char *name);
bool        ir_instr_operand_out(ir_instr_t *in);
bool        ir_instr_operand_fresh(ir_instr_t *in, ir_value_t *storage);
bool        ir_instr_operand_missing(ir_instr_t *in);
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
