// n00b_type_map.c — Translate n00b type-checker types to MIR codegen type tags.

#include "n00b/n00b_type_map.h"
#include "typecheck/unify.h"
#include "adt/variant.h"
#include "core/string.h"
#include "text/strings/string_ops.h"

n00b_cg_type_tag_t
n00b_type_map(n00b_cg_session_t *session, n00b_tc_type_t *type)
{
    (void)session;

    if (!type) {
        return N00B_CG_I64;
    }

    // Chase union-find to canonical representative.
    type = n00b_tc_find(type);

    // Var (unresolved): boxed into n00b_any_t, passed as pointer.
    if (n00b_variant_is_type(type->kind, n00b_tc_var_t)) {
        return N00B_CG_PTR;
    }

    // Primitive: match by interned name.
    if (n00b_variant_is_type(type->kind, n00b_tc_prim_t)) {
        n00b_tc_prim_t prim = n00b_variant_get(type->kind, n00b_tc_prim_t);

        n00b_string_t *name = prim.name;

        if (!name || name->u8_bytes == 0) {
            return N00B_CG_I64;
        }

        if (n00b_unicode_str_eq(name, r"int") || n00b_unicode_str_eq(name, r"i64")) {
            return N00B_CG_I64;
        }
        if (n00b_unicode_str_eq(name, r"i8")) {
            return N00B_CG_I8;
        }
        if (n00b_unicode_str_eq(name, r"i16")) {
            return N00B_CG_I16;
        }
        if (n00b_unicode_str_eq(name, r"i32")) {
            return N00B_CG_I32;
        }
        if (n00b_unicode_str_eq(name, r"u8")) {
            return N00B_CG_U8;
        }
        if (n00b_unicode_str_eq(name, r"u16")) {
            return N00B_CG_U16;
        }
        if (n00b_unicode_str_eq(name, r"u32")) {
            return N00B_CG_U32;
        }
        if (n00b_unicode_str_eq(name, r"u64")) {
            return N00B_CG_U64;
        }
        if (n00b_unicode_str_eq(name, r"f32")) {
            return N00B_CG_F32;
        }
        if (n00b_unicode_str_eq(name, r"f64")) {
            return N00B_CG_F64;
        }
        if (n00b_unicode_str_eq(name, r"bool")) {
            return N00B_CG_BOOL;
        }
        if (n00b_unicode_str_eq(name, r"nil")) {
            return N00B_CG_NIL;
        }
        if (n00b_unicode_str_eq(name, r"void")) {
            return N00B_CG_VOID;
        }
        if (n00b_unicode_str_eq(name, r"string")) {
            return N00B_CG_STRING;
        }

        // Unknown primitive: default to I64.
        return N00B_CG_I64;
    }

    // Parameterized types: dispatch by constructor name.
    if (n00b_variant_is_type(type->kind, n00b_tc_param_t)) {
        n00b_tc_param_t param = n00b_variant_get(type->kind, n00b_tc_param_t);

        if (param.name) {
            if (n00b_unicode_str_eq(param.name, r"list")) {
                return N00B_CG_LIST;
            }
            if (n00b_unicode_str_eq(param.name, r"dict")) {
                return N00B_CG_DICT;
            }
            if (n00b_unicode_str_eq(param.name, r"set")) {
                return N00B_CG_SET;
            }
            if (n00b_unicode_str_eq(param.name, r"result")) {
                return N00B_CG_RESULT;
            }
            if (n00b_unicode_str_eq(param.name, r"maybe")
                || n00b_unicode_str_eq(param.name, r"option")) {
                return N00B_CG_OPTION;
            }
        }

        return N00B_CG_PTR;
    }

    // Function types.
    if (n00b_variant_is_type(type->kind, n00b_tc_fn_t)) {
        return N00B_CG_FUNC;
    }

    // Record, Tuple, Sum: heap-allocated, generic pointer for now.
    if (n00b_variant_is_type(type->kind, n00b_tc_record_t)
        || n00b_variant_is_type(type->kind, n00b_tc_tuple_t)
        || n00b_variant_is_type(type->kind, n00b_tc_sum_t)) {
        return N00B_CG_PTR;
    }

    // Fallback.
    return N00B_CG_I64;
}
