// n00b_compile.c — N00b-specific compilation pipeline orchestrator.
//
// Wires the generic slay annotation walk to n00b-specific callbacks
// (type-spec translator, tokenizer). Registers built-in function
// signatures so the type inference resolves them correctly.

#include "n00b/n00b_compile.h"
#include "slay/infer.h"
#include "slay/symtab.h"
#include "typecheck/construct.h"
#include "typecheck/context.h"
#include "core/string.h"

// ============================================================================
// Register n00b built-in function signatures in the symbol table.
//
// This allows the annotation walk's @infer("return_of($0)") to resolve
// the return types of builtins like some(), ok(), print(), len(), etc.
// ============================================================================

static void
register_builtin_fn(n00b_symtab_t  *st,
                    n00b_tc_ctx_t  *tc,
                    const char     *name,
                    n00b_tc_type_t *fn_type)
{
    n00b_string_t    *n   = n00b_string_from_cstr(name);
    n00b_sym_entry_t *sym = n00b_symtab_add(st, n00b_string_empty(), n,
                                              N00B_SYM_FUNCTION, NULL);
    sym->type_var  = fn_type;
    sym->mutability = N00B_SYM_CONST;
}

static void
register_n00b_builtins(n00b_symtab_t *st, n00b_tc_ctx_t *tc)
{
    // Cached primitives.
    n00b_tc_type_t *t_int    = n00b_tc_prim(tc, r"int");
    n00b_tc_type_t *t_void   = n00b_tc_prim(tc, r"void");

    // print() and len() accept any type. The @call handler in
    // annot_types.c instantiates the callee's type per call site,
    // so a Var param here won't over-constrain across calls.
    {
        n00b_tc_type_t *t_arg = n00b_tc_fresh_var(tc);
        register_builtin_fn(st, tc, "print",
            n00b_tc_fn(tc, t_arg,
                       kw_func(n00b_tc_fn, .returns = t_void)));
    }
    {
        n00b_tc_type_t *t_arg = n00b_tc_fresh_var(tc);
        register_builtin_fn(st, tc, "len",
            n00b_tc_fn(tc, t_arg,
                       kw_func(n00b_tc_fn, .returns = t_int)));
    }

    // some(x) -> option[x]
    n00b_tc_type_t *t_some_a = n00b_tc_fresh_var(tc);
    n00b_tc_type_t *t_option_some_a = n00b_tc_param(tc, r"option", t_some_a);
    register_builtin_fn(st, tc, "some",
        n00b_tc_fn(tc, t_some_a, kw_func(n00b_tc_fn, .returns = t_option_some_a)));

    // none() -> option[`t]  (takes no args — use a fresh var as dummy)
    n00b_tc_type_t *t_none_ret = n00b_tc_param(tc, r"option", n00b_tc_fresh_var(tc));
    register_builtin_fn(st, tc, "none",
        n00b_tc_fn(tc, t_void, kw_func(n00b_tc_fn, .returns = t_none_ret)));

    // ok(x) -> result[x]
    n00b_tc_type_t *t_ok_a = n00b_tc_fresh_var(tc);
    n00b_tc_type_t *t_result_ok_a = n00b_tc_param(tc, r"result", t_ok_a);
    register_builtin_fn(st, tc, "ok",
        n00b_tc_fn(tc, t_ok_a, kw_func(n00b_tc_fn, .returns = t_result_ok_a)));

    // err(x) -> result[`t]
    n00b_tc_type_t *t_err_arg = n00b_tc_fresh_var(tc);
    n00b_tc_type_t *t_err_ret = n00b_tc_param(tc, r"result", n00b_tc_fresh_var(tc));
    register_builtin_fn(st, tc, "err",
        n00b_tc_fn(tc, t_err_arg, kw_func(n00b_tc_fn, .returns = t_err_ret)));
}

n00b_annot_result_t *
n00b_compile_walk(n00b_grammar_t *g, n00b_parse_tree_t *tree)
{
    // Create symtab and type context, register builtins, then walk.
    n00b_symtab_t *st = n00b_symtab_new();
    n00b_tc_ctx_t *tc = n00b_tc_ctx_new();

    register_n00b_builtins(st, tc);

    return n00b_annot_walk_tree_full_ex(g, tree, n00b_tc_translate_type_spec,
                                         .symtab = st, .tc_ctx = tc);
}

n00b_annot_result_t *
n00b_compile_walk_result(n00b_parse_result_t *result)
{
    if (!result || !n00b_parse_result_ok(result)) {
        return NULL;
    }

    n00b_grammar_t    *g    = n00b_parse_result_grammar(result);
    n00b_parse_tree_t *tree = n00b_parse_result_tree(result);

    return n00b_compile_walk(g, tree);
}
