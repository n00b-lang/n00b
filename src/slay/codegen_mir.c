/**
 * @file codegen_mir.c
 * @brief MIR builder helpers: type mapping, instruction emission.
 *
 * All direct MIR API interaction is concentrated here. The codegen
 * engine (codegen.c) calls these helpers to emit instructions.
 *
 * Functions access per-module state (cur_func, labels, temps) through
 * session->active_module, and the MIR context through session->mir_ctx.
 */

#include "n00b.h"
#include "internal/slay/codegen_internal.h"
#include "slay/codegen_builtins.h"
#include "core/alloc.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ============================================================================
// Type mapping
// ============================================================================

MIR_type_t
n00b_cg_mir_type(n00b_cg_type_tag_t tag)
{
    switch (tag) {
    case N00B_CG_I8:
        return MIR_T_I8;
    case N00B_CG_I16:
        return MIR_T_I16;
    case N00B_CG_I32:
        return MIR_T_I32;
    case N00B_CG_I64:
        return MIR_T_I64;
    case N00B_CG_U8:
        return MIR_T_U8;
    case N00B_CG_U16:
        return MIR_T_U16;
    case N00B_CG_U32:
        return MIR_T_U32;
    case N00B_CG_U64:
        return MIR_T_U64;
    case N00B_CG_F32:
        return MIR_T_F;
    case N00B_CG_F64:
        return MIR_T_D;
    // All pointer-like types use I64 at the MIR level. On 64-bit
    // architectures pointers fit in i64, and MIR's type verifier is
    // strict about mixing MIR_T_P with MIR_T_I64 within a function.
    // The semantic pointer distinction is carried by n00b_cg_val_t's
    // type_tag field, not by MIR's type system.
    case N00B_CG_STRING:
    case N00B_CG_LIST:
    case N00B_CG_DICT:
    case N00B_CG_SET:
    case N00B_CG_RESULT:
    case N00B_CG_OPTION:
    case N00B_CG_FUNC:
    case N00B_CG_NIL:
    case N00B_CG_PTR:
        return MIR_T_I64;
    case N00B_CG_BOOL:
        return MIR_T_I64;
    case N00B_CG_VOID:
        return MIR_T_I64; // No void MIR type; use I64 placeholder.
    }

    return MIR_T_I64;
}

// ============================================================================
// Operand construction
// ============================================================================

MIR_op_t
n00b_cg_mir_op(n00b_cg_session_t *s, n00b_cg_val_t val)
{
    n00b_cg_module_t *m = s->active_module;

    switch (val.kind) {
    case N00B_CG_VAL_REG:
        return MIR_new_reg_op(s->mir_ctx, (MIR_reg_t)val.id);

    case N00B_CG_VAL_IMM:
        switch (val.type_tag) {
        case N00B_CG_F32: {
            float f;
            memcpy(&f, &val.aux, sizeof(float));
            return MIR_new_float_op(s->mir_ctx, f);
        }
        case N00B_CG_F64: {
            double d;
            memcpy(&d, &val.aux, sizeof(double));
            return MIR_new_double_op(s->mir_ctx, d);
        }
        default:
            return MIR_new_int_op(s->mir_ctx, (int64_t)val.aux);
        }

    case N00B_CG_VAL_LABEL:
        return MIR_new_label_op(s->mir_ctx, m->labels[val.id]);

    case N00B_CG_VAL_MEM: {
        MIR_type_t mt = n00b_cg_mir_type(val.type_tag);
        return MIR_new_mem_op(s->mir_ctx, mt, (int64_t)val.aux, (MIR_reg_t)val.id, 0, 1);
    }

    case N00B_CG_VAL_VOID:
    default:
        return MIR_new_int_op(s->mir_ctx, 0);
    }
}

// ============================================================================
// Temporary register allocation
// ============================================================================

n00b_cg_val_t
n00b_cg_temp(n00b_cg_session_t *s, n00b_cg_type_tag_t type)
{
    n00b_cg_module_t *m = s->active_module;

    char name[32];
    snprintf(name, sizeof(name), "_t%d", m->temp_counter++);

    MIR_type_t mt  = n00b_cg_mir_type(type);
    MIR_reg_t  reg = MIR_new_func_reg(s->mir_ctx, m->cur_func->u.func, mt, name);

    return (n00b_cg_val_t){
        .id       = (uint32_t)reg,
        .kind     = N00B_CG_VAL_REG,
        .type_tag = type,
    };
}

// ============================================================================
// Binary operation dispatch table
// ============================================================================

static inline bool
is_signed_int(n00b_cg_type_tag_t t)
{
    return t == N00B_CG_I8 || t == N00B_CG_I16 || t == N00B_CG_I32 || t == N00B_CG_I64
        || t == N00B_CG_BOOL;
}

static inline bool
is_unsigned_int(n00b_cg_type_tag_t t)
{
    return t == N00B_CG_U8 || t == N00B_CG_U16 || t == N00B_CG_U32 || t == N00B_CG_U64;
}

static MIR_insn_code_t
binop_insn(n00b_cg_semantic_op_t op, n00b_cg_type_tag_t type)
{
    if (type == N00B_CG_F64) {
        switch (op) {
        case N00B_CG_OP_ADD:
            return MIR_DADD;
        case N00B_CG_OP_SUB:
            return MIR_DSUB;
        case N00B_CG_OP_MUL:
            return MIR_DMUL;
        case N00B_CG_OP_DIV:
            return MIR_DDIV;
        case N00B_CG_OP_EQ:
            return MIR_DEQ;
        case N00B_CG_OP_NE:
            return MIR_DNE;
        case N00B_CG_OP_LT:
            return MIR_DLT;
        case N00B_CG_OP_LE:
            return MIR_DLE;
        case N00B_CG_OP_GT:
            return MIR_DGT;
        case N00B_CG_OP_GE:
            return MIR_DGE;
        default:
            return MIR_INVALID_INSN;
        }
    }

    if (type == N00B_CG_F32) {
        switch (op) {
        case N00B_CG_OP_ADD:
            return MIR_FADD;
        case N00B_CG_OP_SUB:
            return MIR_FSUB;
        case N00B_CG_OP_MUL:
            return MIR_FMUL;
        case N00B_CG_OP_DIV:
            return MIR_FDIV;
        case N00B_CG_OP_EQ:
            return MIR_FEQ;
        case N00B_CG_OP_NE:
            return MIR_FNE;
        case N00B_CG_OP_LT:
            return MIR_FLT;
        case N00B_CG_OP_LE:
            return MIR_FLE;
        case N00B_CG_OP_GT:
            return MIR_FGT;
        case N00B_CG_OP_GE:
            return MIR_FGE;
        default:
            return MIR_INVALID_INSN;
        }
    }

    if (is_unsigned_int(type)) {
        switch (op) {
        case N00B_CG_OP_ADD:
            return MIR_ADD;
        case N00B_CG_OP_SUB:
            return MIR_SUB;
        case N00B_CG_OP_MUL:
            return MIR_MUL;
        case N00B_CG_OP_DIV:
            return MIR_UDIV;
        case N00B_CG_OP_MOD:
            return MIR_UMOD;
        case N00B_CG_OP_AND:
            return MIR_AND;
        case N00B_CG_OP_OR:
            return MIR_OR;
        case N00B_CG_OP_XOR:
            return MIR_XOR;
        case N00B_CG_OP_SHL:
            return MIR_LSH;
        case N00B_CG_OP_SHR:
            return MIR_URSH;
        case N00B_CG_OP_EQ:
            return MIR_EQ;
        case N00B_CG_OP_NE:
            return MIR_NE;
        case N00B_CG_OP_LT:
            return MIR_ULT;
        case N00B_CG_OP_LE:
            return MIR_ULE;
        case N00B_CG_OP_GT:
            return MIR_UGT;
        case N00B_CG_OP_GE:
            return MIR_UGE;
        default:
            return MIR_INVALID_INSN;
        }
    }

    // Signed integer (and bool/ptr).
    switch (op) {
    case N00B_CG_OP_ADD:
        return MIR_ADD;
    case N00B_CG_OP_SUB:
        return MIR_SUB;
    case N00B_CG_OP_MUL:
        return MIR_MUL;
    case N00B_CG_OP_DIV:
        return MIR_DIV;
    case N00B_CG_OP_MOD:
        return MIR_MOD;
    case N00B_CG_OP_AND:
        return MIR_AND;
    case N00B_CG_OP_OR:
        return MIR_OR;
    case N00B_CG_OP_XOR:
        return MIR_XOR;
    case N00B_CG_OP_SHL:
        return MIR_LSH;
    case N00B_CG_OP_SHR:
        return MIR_RSH;
    case N00B_CG_OP_EQ:
        return MIR_EQ;
    case N00B_CG_OP_NE:
        return MIR_NE;
    case N00B_CG_OP_LT:
        return MIR_LT;
    case N00B_CG_OP_LE:
        return MIR_LE;
    case N00B_CG_OP_GT:
        return MIR_GT;
    case N00B_CG_OP_GE:
        return MIR_GE;
    default:
        return MIR_INVALID_INSN;
    }
}

// ============================================================================
// Binary operation emission
// ============================================================================

n00b_cg_val_t
n00b_cg_emit_binop(n00b_cg_session_t    *s,
                   n00b_cg_semantic_op_t op,
                   n00b_cg_val_t         a,
                   n00b_cg_val_t         b)
{
    // String operations: dispatch to C runtime helpers instead of
    // MIR arithmetic instructions.
    if (a.type_tag == N00B_CG_STRING) {
        switch (op) {
        case N00B_CG_OP_ADD: {
            n00b_cg_type_tag_t pt[] = {N00B_CG_I64, N00B_CG_I64};
            n00b_cg_import_func(s,
                                "n00b_builtin_str_concat",
                                (void *)n00b_builtin_str_concat,
                                .ret         = N00B_CG_I64,
                                .param_types = pt,
                                .n_params    = 2);
            n00b_cg_val_t args[] = {a, b};
            n00b_cg_val_t r
                = n00b_cg_emit_call(s, "n00b_builtin_str_concat", args, 2, .ret = N00B_CG_I64);
            r.type_tag = N00B_CG_STRING;
            return r;
        }
        case N00B_CG_OP_EQ: {
            n00b_cg_type_tag_t pt[] = {N00B_CG_I64, N00B_CG_I64};
            n00b_cg_import_func(s,
                                "n00b_builtin_str_eq",
                                (void *)n00b_builtin_str_eq,
                                .ret         = N00B_CG_I64,
                                .param_types = pt,
                                .n_params    = 2);
            n00b_cg_val_t args[] = {a, b};
            return n00b_cg_emit_call(s, "n00b_builtin_str_eq", args, 2, .ret = N00B_CG_I64);
        }
        case N00B_CG_OP_NE: {
            n00b_cg_type_tag_t pt[] = {N00B_CG_I64, N00B_CG_I64};
            n00b_cg_import_func(s,
                                "n00b_builtin_str_eq",
                                (void *)n00b_builtin_str_eq,
                                .ret         = N00B_CG_I64,
                                .param_types = pt,
                                .n_params    = 2);
            n00b_cg_val_t args[] = {a, b};
            n00b_cg_val_t eq
                = n00b_cg_emit_call(s, "n00b_builtin_str_eq", args, 2, .ret = N00B_CG_I64);
            return n00b_cg_emit_unop(s, N00B_CG_OP_LOGICAL_NOT, eq);
        }
        default:
            return N00B_CG_VOID_VAL;
        }
    }

    n00b_cg_module_t  *m           = s->active_module;
    n00b_cg_type_tag_t result_type = a.type_tag;

    if (op >= N00B_CG_OP_EQ && op <= N00B_CG_OP_GE) {
        result_type = N00B_CG_I64;
    }

    MIR_insn_code_t insn = binop_insn(op, a.type_tag);

    if (insn == MIR_INVALID_INSN) {
        return N00B_CG_VOID_VAL;
    }

    n00b_cg_val_t dst = n00b_cg_temp(s, result_type);

    MIR_append_insn(s->mir_ctx,
                    m->cur_func,
                    MIR_new_insn(s->mir_ctx,
                                 insn,
                                 n00b_cg_mir_op(s, dst),
                                 n00b_cg_mir_op(s, a),
                                 n00b_cg_mir_op(s, b)));

    return dst;
}

// ============================================================================
// Unary operation emission
// ============================================================================

n00b_cg_val_t
n00b_cg_emit_unop(n00b_cg_session_t *s, n00b_cg_semantic_op_t op, n00b_cg_val_t a)
{
    n00b_cg_module_t *m   = s->active_module;
    n00b_cg_val_t     dst = n00b_cg_temp(s, a.type_tag);

    MIR_insn_code_t insn;

    switch (op) {
    case N00B_CG_OP_NEG:
        if (a.type_tag == N00B_CG_F64) {
            insn = MIR_DNEG;
        }
        else if (a.type_tag == N00B_CG_F32) {
            insn = MIR_FNEG;
        }
        else {
            insn = MIR_NEG;
        }
        break;

    case N00B_CG_OP_NOT:
    case N00B_CG_OP_LOGICAL_NOT: {
        if (op == N00B_CG_OP_LOGICAL_NOT) {
            dst.type_tag = N00B_CG_I64;
            MIR_append_insn(s->mir_ctx,
                            m->cur_func,
                            MIR_new_insn(s->mir_ctx,
                                         MIR_EQ,
                                         n00b_cg_mir_op(s, dst),
                                         n00b_cg_mir_op(s, a),
                                         MIR_new_int_op(s->mir_ctx, 0)));
            return dst;
        }
        MIR_append_insn(s->mir_ctx,
                        m->cur_func,
                        MIR_new_insn(s->mir_ctx,
                                     MIR_XOR,
                                     n00b_cg_mir_op(s, dst),
                                     n00b_cg_mir_op(s, a),
                                     MIR_new_int_op(s->mir_ctx, -1)));
        return dst;
    }

    default:
        return N00B_CG_VOID_VAL;
    }

    MIR_append_insn(
        s->mir_ctx,
        m->cur_func,
        MIR_new_insn(s->mir_ctx, insn, n00b_cg_mir_op(s, dst), n00b_cg_mir_op(s, a)));

    return dst;
}

// ============================================================================
// Public builder: Arithmetic / logic
// ============================================================================

n00b_cg_val_t
n00b_cg_emit_add(n00b_cg_session_t *s, n00b_cg_val_t a, n00b_cg_val_t b)
{
    return n00b_cg_emit_binop(s, N00B_CG_OP_ADD, a, b);
}

n00b_cg_val_t
n00b_cg_emit_sub(n00b_cg_session_t *s, n00b_cg_val_t a, n00b_cg_val_t b)
{
    return n00b_cg_emit_binop(s, N00B_CG_OP_SUB, a, b);
}

n00b_cg_val_t
n00b_cg_emit_mul(n00b_cg_session_t *s, n00b_cg_val_t a, n00b_cg_val_t b)
{
    return n00b_cg_emit_binop(s, N00B_CG_OP_MUL, a, b);
}

n00b_cg_val_t
n00b_cg_emit_div(n00b_cg_session_t *s, n00b_cg_val_t a, n00b_cg_val_t b)
{
    return n00b_cg_emit_binop(s, N00B_CG_OP_DIV, a, b);
}

n00b_cg_val_t
n00b_cg_emit_mod(n00b_cg_session_t *s, n00b_cg_val_t a, n00b_cg_val_t b)
{
    return n00b_cg_emit_binop(s, N00B_CG_OP_MOD, a, b);
}

n00b_cg_val_t
n00b_cg_emit_neg(n00b_cg_session_t *s, n00b_cg_val_t a)
{
    return n00b_cg_emit_unop(s, N00B_CG_OP_NEG, a);
}

n00b_cg_val_t
n00b_cg_emit_and(n00b_cg_session_t *s, n00b_cg_val_t a, n00b_cg_val_t b)
{
    return n00b_cg_emit_binop(s, N00B_CG_OP_AND, a, b);
}

n00b_cg_val_t
n00b_cg_emit_or(n00b_cg_session_t *s, n00b_cg_val_t a, n00b_cg_val_t b)
{
    return n00b_cg_emit_binop(s, N00B_CG_OP_OR, a, b);
}

n00b_cg_val_t
n00b_cg_emit_xor(n00b_cg_session_t *s, n00b_cg_val_t a, n00b_cg_val_t b)
{
    return n00b_cg_emit_binop(s, N00B_CG_OP_XOR, a, b);
}

n00b_cg_val_t
n00b_cg_emit_not(n00b_cg_session_t *s, n00b_cg_val_t a)
{
    return n00b_cg_emit_unop(s, N00B_CG_OP_NOT, a);
}

n00b_cg_val_t
n00b_cg_emit_shl(n00b_cg_session_t *s, n00b_cg_val_t a, n00b_cg_val_t b)
{
    return n00b_cg_emit_binop(s, N00B_CG_OP_SHL, a, b);
}

n00b_cg_val_t
n00b_cg_emit_shr(n00b_cg_session_t *s, n00b_cg_val_t a, n00b_cg_val_t b)
{
    return n00b_cg_emit_binop(s, N00B_CG_OP_SHR, a, b);
}

n00b_cg_val_t
n00b_cg_emit_eq(n00b_cg_session_t *s, n00b_cg_val_t a, n00b_cg_val_t b)
{
    return n00b_cg_emit_binop(s, N00B_CG_OP_EQ, a, b);
}

n00b_cg_val_t
n00b_cg_emit_ne(n00b_cg_session_t *s, n00b_cg_val_t a, n00b_cg_val_t b)
{
    return n00b_cg_emit_binop(s, N00B_CG_OP_NE, a, b);
}

n00b_cg_val_t
n00b_cg_emit_lt(n00b_cg_session_t *s, n00b_cg_val_t a, n00b_cg_val_t b)
{
    return n00b_cg_emit_binop(s, N00B_CG_OP_LT, a, b);
}

n00b_cg_val_t
n00b_cg_emit_le(n00b_cg_session_t *s, n00b_cg_val_t a, n00b_cg_val_t b)
{
    return n00b_cg_emit_binop(s, N00B_CG_OP_LE, a, b);
}

n00b_cg_val_t
n00b_cg_emit_gt(n00b_cg_session_t *s, n00b_cg_val_t a, n00b_cg_val_t b)
{
    return n00b_cg_emit_binop(s, N00B_CG_OP_GT, a, b);
}

n00b_cg_val_t
n00b_cg_emit_ge(n00b_cg_session_t *s, n00b_cg_val_t a, n00b_cg_val_t b)
{
    return n00b_cg_emit_binop(s, N00B_CG_OP_GE, a, b);
}

// ============================================================================
// Type conversion
// ============================================================================

n00b_cg_val_t
n00b_cg_emit_convert(n00b_cg_session_t *s, n00b_cg_val_t src, n00b_cg_type_tag_t dst_type)
{
    if (src.type_tag == dst_type) {
        return src;
    }

    n00b_cg_module_t *m   = s->active_module;
    n00b_cg_val_t     dst = n00b_cg_temp(s, dst_type);
    MIR_insn_code_t   insn;

    bool src_int = is_signed_int(src.type_tag) || is_unsigned_int(src.type_tag);
    bool dst_int = is_signed_int(dst_type) || is_unsigned_int(dst_type);

    if (src_int && dst_type == N00B_CG_F64) {
        insn = is_unsigned_int(src.type_tag) ? MIR_UI2D : MIR_I2D;
    }
    else if (src_int && dst_type == N00B_CG_F32) {
        insn = is_unsigned_int(src.type_tag) ? MIR_UI2F : MIR_I2F;
    }
    else if (src.type_tag == N00B_CG_F64 && dst_int) {
        insn = MIR_D2I;
    }
    else if (src.type_tag == N00B_CG_F32 && dst_int) {
        insn = MIR_F2I;
    }
    else if (src.type_tag == N00B_CG_F32 && dst_type == N00B_CG_F64) {
        insn = MIR_F2D;
    }
    else if (src.type_tag == N00B_CG_F64 && dst_type == N00B_CG_F32) {
        insn = MIR_D2F;
    }
    else {
        insn = MIR_MOV;
    }

    MIR_append_insn(
        s->mir_ctx,
        m->cur_func,
        MIR_new_insn(s->mir_ctx, insn, n00b_cg_mir_op(s, dst), n00b_cg_mir_op(s, src)));

    return dst;
}

// ============================================================================
// Constants
// ============================================================================

n00b_cg_val_t
_n00b_cg_const_i64(n00b_cg_session_t *s, int64_t v)
{
    (void)s;
    return (n00b_cg_val_t){
        .kind     = N00B_CG_VAL_IMM,
        .type_tag = N00B_CG_I64,
        .aux      = (uint64_t)v,
    };
}

n00b_cg_val_t
_n00b_cg_const_i32(n00b_cg_session_t *s, int32_t v)
{
    (void)s;
    return (n00b_cg_val_t){
        .kind     = N00B_CG_VAL_IMM,
        .type_tag = N00B_CG_I32,
        .aux      = (uint64_t)(int64_t)v,
    };
}

n00b_cg_val_t
_n00b_cg_const_u64(n00b_cg_session_t *s, uint64_t v)
{
    (void)s;
    return (n00b_cg_val_t){
        .kind     = N00B_CG_VAL_IMM,
        .type_tag = N00B_CG_U64,
        .aux      = v,
    };
}

n00b_cg_val_t
_n00b_cg_const_f64(n00b_cg_session_t *s, double v)
{
    (void)s;
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return (n00b_cg_val_t){
        .kind     = N00B_CG_VAL_IMM,
        .type_tag = N00B_CG_F64,
        .aux      = bits,
    };
}

n00b_cg_val_t
_n00b_cg_const_f32(n00b_cg_session_t *s, float v)
{
    (void)s;
    uint64_t bits = 0;
    memcpy(&bits, &v, sizeof(float));
    return (n00b_cg_val_t){
        .kind     = N00B_CG_VAL_IMM,
        .type_tag = N00B_CG_F32,
        .aux      = bits,
    };
}

n00b_cg_val_t
_n00b_cg_const_bool(n00b_cg_session_t *s, bool v)
{
    (void)s;
    return (n00b_cg_val_t){
        .kind     = N00B_CG_VAL_IMM,
        .type_tag = N00B_CG_BOOL,
        .aux      = v ? 1 : 0,
    };
}

n00b_cg_val_t
_n00b_cg_const_ptr(n00b_cg_session_t *s, void *v)
{
    (void)s;
    return (n00b_cg_val_t){
        .kind     = N00B_CG_VAL_IMM,
        .type_tag = N00B_CG_PTR,
        .aux      = (uint64_t)(uintptr_t)v,
    };
}

// ============================================================================
// Labels
// ============================================================================

n00b_cg_val_t
n00b_cg_label_new(n00b_cg_session_t *s)
{
    n00b_cg_module_t *m = s->active_module;

    if (m->label_count >= m->label_cap) {
        int32_t      new_cap    = m->label_cap ? m->label_cap * 2 : 16;
        MIR_label_t *new_labels = n00b_alloc_array(MIR_label_t, (size_t)new_cap);
        if (m->labels) {
            memcpy(new_labels, m->labels, sizeof(MIR_label_t) * (size_t)m->label_count);
        }
        m->labels    = new_labels;
        m->label_cap = new_cap;
    }

    MIR_label_t lbl = MIR_new_label(s->mir_ctx);
    int32_t     idx = m->label_count++;

    m->labels[idx] = lbl;

    return (n00b_cg_val_t){
        .id       = (uint32_t)idx,
        .kind     = N00B_CG_VAL_LABEL,
        .type_tag = N00B_CG_VOID,
    };
}

void
n00b_cg_label_here(n00b_cg_session_t *s, n00b_cg_val_t label)
{
    n00b_cg_module_t *m = s->active_module;
    MIR_append_insn(s->mir_ctx, m->cur_func, m->labels[label.id]);
}

// ============================================================================
// Control flow
// ============================================================================

void
n00b_cg_emit_jmp(n00b_cg_session_t *s, n00b_cg_val_t label)
{
    n00b_cg_module_t *m = s->active_module;
    MIR_append_insn(
        s->mir_ctx,
        m->cur_func,
        MIR_new_insn(s->mir_ctx, MIR_JMP, MIR_new_label_op(s->mir_ctx, m->labels[label.id])));
}

void
n00b_cg_emit_bt(n00b_cg_session_t *s, n00b_cg_val_t cond, n00b_cg_val_t label)
{
    n00b_cg_module_t *m = s->active_module;
    MIR_append_insn(s->mir_ctx,
                    m->cur_func,
                    MIR_new_insn(s->mir_ctx,
                                 MIR_BT,
                                 MIR_new_label_op(s->mir_ctx, m->labels[label.id]),
                                 n00b_cg_mir_op(s, cond)));
}

void
n00b_cg_emit_bf(n00b_cg_session_t *s, n00b_cg_val_t cond, n00b_cg_val_t label)
{
    n00b_cg_module_t *m = s->active_module;
    MIR_append_insn(s->mir_ctx,
                    m->cur_func,
                    MIR_new_insn(s->mir_ctx,
                                 MIR_BF,
                                 MIR_new_label_op(s->mir_ctx, m->labels[label.id]),
                                 n00b_cg_mir_op(s, cond)));
}

void
n00b_cg_emit_ret(n00b_cg_session_t *s, n00b_cg_val_t val)
{
    n00b_cg_module_t *m = s->active_module;
    MIR_append_insn(s->mir_ctx,
                    m->cur_func,
                    MIR_new_ret_insn(s->mir_ctx, 1, n00b_cg_mir_op(s, val)));
}

void
n00b_cg_emit_ret_void(n00b_cg_session_t *s)
{
    n00b_cg_module_t *m = s->active_module;
    MIR_append_insn(s->mir_ctx, m->cur_func, MIR_new_ret_insn(s->mir_ctx, 0));
}

// ============================================================================
// Function scope
// ============================================================================

void
n00b_cg_begin_func(n00b_cg_session_t *s, const char *name) _kargs
{
    n00b_cg_type_tag_t  ret;
    const char        **param_names;
    n00b_cg_type_tag_t *param_types;
    int32_t             n_params;
    bool                is_vararg;
}
{
    n00b_cg_module_t *m = s->active_module;

    MIR_type_t res_type = n00b_cg_mir_type(kargs->ret);
    int        nres     = (kargs->ret == N00B_CG_VOID) ? 0 : 1;

    int32_t np = kargs->n_params;

    if (np == 0) {
        m->cur_func = MIR_new_func(s->mir_ctx, name, nres, &res_type, 0);
    }
    else {
        MIR_var_t *vars = n00b_alloc_array(MIR_var_t, (size_t)np);

        for (int32_t i = 0; i < np; i++) {
            vars[i].type
                = n00b_cg_mir_type(kargs->param_types ? kargs->param_types[i] : N00B_CG_I64);
            vars[i].name = kargs->param_names ? kargs->param_names[i] : "arg";
            vars[i].size = 0;
        }

        m->cur_func = MIR_new_func_arr(s->mir_ctx, name, nres, &res_type, np, vars);
    }

    m->temp_counter = 0;
    m->label_count  = 0;
    m->loop_depth   = 0;
}

void
n00b_cg_end_func(n00b_cg_session_t *s)
{
    MIR_finish_func(s->mir_ctx);
    s->active_module->cur_func = NULL;
}

n00b_cg_val_t
n00b_cg_param(n00b_cg_session_t *s, int32_t index)
{
    n00b_cg_module_t *m    = s->active_module;
    MIR_func_t        func = m->cur_func->u.func;

    if (index < 0 || (size_t)index >= func->nargs) {
        return N00B_CG_VOID_VAL;
    }

    MIR_var_t var = VARR_GET(MIR_var_t, func->vars, index);
    MIR_reg_t reg = MIR_reg(s->mir_ctx, var.name, func);

    n00b_cg_type_tag_t tag = N00B_CG_I64;

    switch (var.type) {
    case MIR_T_I8:
        tag = N00B_CG_I8;
        break;
    case MIR_T_I16:
        tag = N00B_CG_I16;
        break;
    case MIR_T_I32:
        tag = N00B_CG_I32;
        break;
    case MIR_T_I64:
        tag = N00B_CG_I64;
        break;
    case MIR_T_U8:
        tag = N00B_CG_U8;
        break;
    case MIR_T_U16:
        tag = N00B_CG_U16;
        break;
    case MIR_T_U32:
        tag = N00B_CG_U32;
        break;
    case MIR_T_U64:
        tag = N00B_CG_U64;
        break;
    case MIR_T_F:
        tag = N00B_CG_F32;
        break;
    case MIR_T_D:
        tag = N00B_CG_F64;
        break;
    case MIR_T_P:
        tag = N00B_CG_PTR;
        break;
    default:
        tag = N00B_CG_I64;
        break;
    }

    return (n00b_cg_val_t){
        .id       = (uint32_t)reg,
        .kind     = N00B_CG_VAL_REG,
        .type_tag = tag,
    };
}

// ============================================================================
// Locals
// ============================================================================

n00b_cg_val_t
n00b_cg_local(n00b_cg_session_t *s, const char *name) _kargs
{
    n00b_cg_type_tag_t type;
}
{
    n00b_cg_module_t  *m   = s->active_module;
    n00b_cg_type_tag_t tt  = kargs->type ? kargs->type : N00B_CG_I64;
    MIR_type_t         mt  = n00b_cg_mir_type(tt);
    MIR_reg_t          reg = MIR_new_func_reg(s->mir_ctx, m->cur_func->u.func, mt, name);

    return (n00b_cg_val_t){
        .id       = (uint32_t)reg,
        .kind     = N00B_CG_VAL_REG,
        .type_tag = tt,
    };
}

void
n00b_cg_store(n00b_cg_session_t *s, n00b_cg_val_t dst, n00b_cg_val_t src)
{
    n00b_cg_module_t *m = s->active_module;
    MIR_insn_code_t   mov;

    if (dst.type_tag == N00B_CG_F64) {
        mov = MIR_DMOV;
    }
    else if (dst.type_tag == N00B_CG_F32) {
        mov = MIR_FMOV;
    }
    else {
        mov = MIR_MOV;
    }

    MIR_append_insn(
        s->mir_ctx,
        m->cur_func,
        MIR_new_insn(s->mir_ctx, mov, n00b_cg_mir_op(s, dst), n00b_cg_mir_op(s, src)));
}

n00b_cg_val_t
n00b_cg_load(n00b_cg_session_t *s, n00b_cg_val_t var)
{
    if (var.kind == N00B_CG_VAL_REG) {
        return var;
    }

    n00b_cg_module_t *m   = s->active_module;
    n00b_cg_val_t     dst = n00b_cg_temp(s, var.type_tag);

    MIR_insn_code_t mov;

    if (var.type_tag == N00B_CG_F64) {
        mov = MIR_DMOV;
    }
    else if (var.type_tag == N00B_CG_F32) {
        mov = MIR_FMOV;
    }
    else {
        mov = MIR_MOV;
    }

    MIR_append_insn(
        s->mir_ctx,
        m->cur_func,
        MIR_new_insn(s->mir_ctx, mov, n00b_cg_mir_op(s, dst), n00b_cg_mir_op(s, var)));

    return dst;
}

// ============================================================================
// Memory operations
// ============================================================================

n00b_cg_val_t
n00b_cg_emit_mem_load(n00b_cg_session_t *s, n00b_cg_val_t addr, n00b_cg_type_tag_t type) _kargs
{
    int64_t offset;
}
{
    n00b_cg_module_t *m   = s->active_module;
    n00b_cg_val_t     dst = n00b_cg_temp(s, type);

    MIR_type_t mt  = n00b_cg_mir_type(type);
    MIR_op_t   mem = MIR_new_mem_op(s->mir_ctx, mt, kargs->offset, (MIR_reg_t)addr.id, 0, 1);

    MIR_insn_code_t mov = (type == N00B_CG_F64) ? MIR_DMOV
                        : (type == N00B_CG_F32) ? MIR_FMOV
                                                : MIR_MOV;

    MIR_append_insn(s->mir_ctx,
                    m->cur_func,
                    MIR_new_insn(s->mir_ctx, mov, n00b_cg_mir_op(s, dst), mem));

    return dst;
}

void
n00b_cg_emit_mem_store(n00b_cg_session_t *s, n00b_cg_val_t addr, n00b_cg_val_t value) _kargs
{
    int64_t offset;
}
{
    n00b_cg_module_t *m = s->active_module;

    MIR_type_t mt  = n00b_cg_mir_type(value.type_tag);
    MIR_op_t   mem = MIR_new_mem_op(s->mir_ctx, mt, kargs->offset, (MIR_reg_t)addr.id, 0, 1);

    MIR_insn_code_t mov = (value.type_tag == N00B_CG_F64) ? MIR_DMOV
                        : (value.type_tag == N00B_CG_F32) ? MIR_FMOV
                                                          : MIR_MOV;

    MIR_append_insn(s->mir_ctx,
                    m->cur_func,
                    MIR_new_insn(s->mir_ctx, mov, mem, n00b_cg_mir_op(s, value)));
}

n00b_cg_val_t
n00b_cg_emit_alloca(n00b_cg_session_t *s, int64_t size)
{
    n00b_cg_module_t *m   = s->active_module;
    n00b_cg_val_t     dst = n00b_cg_temp(s, N00B_CG_PTR);
    n00b_cg_val_t     sz  = _n00b_cg_const_i64(s, size);

    MIR_append_insn(
        s->mir_ctx,
        m->cur_func,
        MIR_new_insn(s->mir_ctx, MIR_ALLOCA, n00b_cg_mir_op(s, dst), n00b_cg_mir_op(s, sz)));

    return dst;
}

// ============================================================================
// Proto caching (per-module)
// ============================================================================

MIR_item_t
n00b_cg_get_or_create_proto(n00b_cg_session_t  *s,
                            const char         *name,
                            n00b_cg_type_tag_t  ret,
                            n00b_cg_type_tag_t *param_types,
                            int32_t             n_params,
                            bool                is_vararg)
{
    n00b_cg_module_t *m = s->active_module;

    // Check existing func protos in the module.
    for (int32_t i = 0; i < m->func_proto_count; i++) {
        if (strcmp(m->func_names[i], name) == 0) {
            return m->func_protos[i];
        }
    }

    // Create a new proto.
    MIR_type_t res_type = n00b_cg_mir_type(ret);
    int        nres     = (ret == N00B_CG_VOID) ? 0 : 1;

    MIR_var_t *vars = NULL;

    if (n_params > 0) {
        vars = n00b_alloc_array(MIR_var_t, (size_t)n_params);
        for (int32_t i = 0; i < n_params; i++) {
            vars[i].type = n00b_cg_mir_type(param_types ? param_types[i] : N00B_CG_I64);
            vars[i].name = "p";
            vars[i].size = 0;
        }
    }

    char proto_name[128];
    snprintf(proto_name, sizeof(proto_name), "_proto_%s", name);

    MIR_item_t proto = MIR_new_proto_arr(s->mir_ctx,
                                         proto_name,
                                         nres,
                                         nres ? &res_type : NULL,
                                         n_params,
                                         vars);

    // Cache it in the module.
    if (m->func_proto_count >= m->func_proto_cap) {
        int32_t      new_cap    = m->func_proto_cap ? m->func_proto_cap * 2 : 16;
        MIR_item_t  *new_protos = n00b_alloc_array(MIR_item_t, (size_t)new_cap);
        const char **new_names  = n00b_alloc_array(const char *, (size_t)new_cap);

        if (m->func_protos) {
            memcpy(new_protos,
                   m->func_protos,
                   sizeof(MIR_item_t) * (size_t)m->func_proto_count);
            memcpy(new_names,
                   m->func_names,
                   sizeof(const char *) * (size_t)m->func_proto_count);
        }

        m->func_protos    = new_protos;
        m->func_names     = new_names;
        m->func_proto_cap = new_cap;
    }

    m->func_protos[m->func_proto_count] = proto;
    m->func_names[m->func_proto_count]  = name;
    m->func_proto_count++;

    (void)is_vararg;

    return proto;
}

// ============================================================================
// Import lookup (searches active module)
// ============================================================================

n00b_cg_import_t *
n00b_cg_find_import(n00b_cg_session_t *s, const char *name)
{
    n00b_cg_module_t *m = s->active_module;

    if (!m) {
        return NULL;
    }

    for (int32_t i = 0; i < m->import_count; i++) {
        if (strcmp(m->imports[i].name, name) == 0) {
            return &m->imports[i];
        }
    }

    return NULL;
}

// ============================================================================
// Function lookup (searches active module, then all modules)
// ============================================================================

MIR_item_t
n00b_cg_find_func(n00b_cg_session_t *s, const char *name)
{
    n00b_cg_module_t *m = s->active_module;

    if (m) {
        MIR_item_t item;
        for (item = DLIST_HEAD(MIR_item_t, m->mir_module->items); item != NULL;
             item = DLIST_NEXT(MIR_item_t, item)) {
            if (item->item_type == MIR_func_item && strcmp(item->u.func->name, name) == 0) {
                return item;
            }
        }
    }

    // Search all modules in the session.
    for (int32_t i = 0; i < s->module_count; i++) {
        n00b_cg_module_t *mod = s->modules[i];

        if (mod == m) {
            continue;
        }

        MIR_item_t item;
        for (item = DLIST_HEAD(MIR_item_t, mod->mir_module->items); item != NULL;
             item = DLIST_NEXT(MIR_item_t, item)) {
            if (item->item_type == MIR_func_item && strcmp(item->u.func->name, name) == 0) {
                if (n00b_cg_module_func_is_private(mod, name)) {
                    continue;
                }

                return item;
            }
        }
    }

    return NULL;
}

// ============================================================================
// Operator lookup
// ============================================================================

int32_t
n00b_cg_lookup_op(n00b_cg_session_t *s, const char *text)
{
    for (int32_t i = 0; i < s->op_map_count; i++) {
        if (strcmp(s->op_map[i].text, text) == 0) {
            return (int32_t)s->op_map[i].op;
        }
    }
    return -1;
}

// ============================================================================
// Function calls
// ============================================================================

n00b_cg_val_t
n00b_cg_emit_call(n00b_cg_session_t *s,
                  const char        *func_name,
                  n00b_cg_val_t     *args,
                  int32_t            n_args) _kargs
{
    n00b_cg_type_tag_t ret;
}
{
    n00b_cg_module_t  *m        = s->active_module;
    n00b_cg_type_tag_t ret_type = kargs->ret;

    n00b_cg_import_t *imp = n00b_cg_find_import(s, func_name);

    MIR_item_t proto;
    MIR_op_t   callee_op;

    if (imp) {
        proto     = imp->proto;
        callee_op = MIR_new_ref_op(s->mir_ctx, imp->import);
    }
    else {
        MIR_item_t func_item = n00b_cg_find_func(s, func_name);

        if (!func_item) {
            return N00B_CG_VOID_VAL;
        }

        MIR_func_t fn = func_item->u.func;

        // Override ret_type based on the actual function's return count.
        // The caller may pass I64 for unresolved types, but the wrapper
        // may be void — trust the function definition.
        if (fn->nres == 0) {
            ret_type = N00B_CG_VOID;
        }

        n00b_cg_type_tag_t *ptypes = NULL;

        if (fn->nargs > 0) {
            ptypes = n00b_alloc_array(n00b_cg_type_tag_t, fn->nargs);
            for (uint32_t i = 0; i < fn->nargs; i++) {
                MIR_var_t fv = VARR_GET(MIR_var_t, fn->vars, i);
                switch (fv.type) {
                case MIR_T_F:
                    ptypes[i] = N00B_CG_F32;
                    break;
                case MIR_T_D:
                    ptypes[i] = N00B_CG_F64;
                    break;
                case MIR_T_P:
                    ptypes[i] = N00B_CG_PTR;
                    break;
                default:
                    ptypes[i] = N00B_CG_I64;
                    break;
                }
            }
        }

        proto     = n00b_cg_get_or_create_proto(s,
                                                func_name,
                                                ret_type,
                                                ptypes,
                                                (int32_t)fn->nargs,
                                                false);
        callee_op = MIR_new_ref_op(s->mir_ctx, func_item);
    }

    bool      has_ret = (ret_type != N00B_CG_VOID);
    int32_t   total   = 2 + (has_ret ? 1 : 0) + n_args;
    MIR_op_t *ops     = n00b_alloc_array(MIR_op_t, (size_t)total);
    int32_t   idx     = 0;

    ops[idx++] = MIR_new_ref_op(s->mir_ctx, proto);
    ops[idx++] = callee_op;

    n00b_cg_val_t result = N00B_CG_VOID_VAL;

    if (has_ret) {
        result     = n00b_cg_temp(s, ret_type);
        ops[idx++] = n00b_cg_mir_op(s, result);
    }

    for (int32_t i = 0; i < n_args; i++) {
        ops[idx++] = n00b_cg_mir_op(s, args[i]);
    }

    MIR_append_insn(s->mir_ctx,
                    m->cur_func,
                    MIR_new_insn_arr(s->mir_ctx, MIR_CALL, (size_t)total, ops));

    return result;
}

n00b_cg_val_t
n00b_cg_emit_call_indirect(n00b_cg_session_t *s,
                           n00b_cg_val_t      func_ptr,
                           n00b_cg_val_t     *args,
                           int32_t            n_args) _kargs
{
    n00b_cg_type_tag_t ret;
}
{
    n00b_cg_module_t  *m        = s->active_module;
    n00b_cg_type_tag_t ret_type = kargs->ret;

    n00b_cg_type_tag_t *ptypes = NULL;

    if (n_args > 0) {
        ptypes = n00b_alloc_array(n00b_cg_type_tag_t, (size_t)n_args);
        for (int32_t i = 0; i < n_args; i++) {
            ptypes[i] = args[i].type_tag;
        }
    }

    MIR_item_t proto
        = n00b_cg_get_or_create_proto(s, "_indirect", ret_type, ptypes, n_args, false);

    bool      has_ret = (ret_type != N00B_CG_VOID);
    int32_t   total   = 2 + (has_ret ? 1 : 0) + n_args;
    MIR_op_t *ops     = n00b_alloc_array(MIR_op_t, (size_t)total);
    int32_t   idx     = 0;

    ops[idx++] = MIR_new_ref_op(s->mir_ctx, proto);
    ops[idx++] = n00b_cg_mir_op(s, func_ptr);

    n00b_cg_val_t result = N00B_CG_VOID_VAL;

    if (has_ret) {
        result     = n00b_cg_temp(s, ret_type);
        ops[idx++] = n00b_cg_mir_op(s, result);
    }

    for (int32_t i = 0; i < n_args; i++) {
        ops[idx++] = n00b_cg_mir_op(s, args[i]);
    }

    MIR_append_insn(s->mir_ctx,
                    m->cur_func,
                    MIR_new_insn_arr(s->mir_ctx, MIR_CALL, (size_t)total, ops));

    return result;
}

// ============================================================================
// Imports (per-module)
// ============================================================================

void
n00b_cg_import_func(n00b_cg_session_t *s, const char *name, void *addr) _kargs
{
    n00b_cg_type_tag_t  ret;
    n00b_cg_type_tag_t *param_types;
    int32_t             n_params;
    bool                is_vararg;
}
{
    n00b_cg_module_t *m = s->active_module;

    // Check if already imported in this module.
    n00b_cg_import_t *existing = n00b_cg_find_import(s, name);

    if (existing) {
        return;
    }

    // Create proto.
    MIR_item_t proto = n00b_cg_get_or_create_proto(s,
                                                   name,
                                                   kargs->ret,
                                                   kargs->param_types,
                                                   kargs->n_params,
                                                   kargs->is_vararg);

    // Create import item.
    MIR_item_t import = MIR_new_import(s->mir_ctx, name);

    // Store in module's imports array.
    if (m->import_count >= m->import_cap) {
        int32_t           new_cap     = m->import_cap ? m->import_cap * 2 : 16;
        n00b_cg_import_t *new_imports = n00b_alloc_array(n00b_cg_import_t, (size_t)new_cap);
        if (m->imports) {
            memcpy(new_imports, m->imports, sizeof(n00b_cg_import_t) * (size_t)m->import_count);
        }
        m->imports    = new_imports;
        m->import_cap = new_cap;
    }

    m->imports[m->import_count++] = (n00b_cg_import_t){
        .name   = name,
        .proto  = proto,
        .import = import,
        .addr   = addr,
    };
}
