/**
 * @file codegen.c
 * @brief Code generation engine: lifecycle, tree walk, auto-inference, audit.
 *
 * This is the core codegen engine that walks a parse tree, dispatching
 * to registered handlers and auto-inferred codegen based on grammar
 * annotations (@operator, @literal, @branch, @loop, etc.).
 */

#include "n00b.h"
#include "internal/slay/codegen_internal.h"
#include "core/alloc.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ============================================================================
// Default built-in operator table
// ============================================================================

static const struct {
    const char           *text;
    n00b_cg_semantic_op_t op;
} default_ops[] = {
    {"+",   N00B_CG_OP_ADD},
    {"-",   N00B_CG_OP_SUB},
    {"*",   N00B_CG_OP_MUL},
    {"/",   N00B_CG_OP_DIV},
    {"%",   N00B_CG_OP_MOD},
    {"&",   N00B_CG_OP_AND},
    {"|",   N00B_CG_OP_OR},
    {"^",   N00B_CG_OP_XOR},
    {"<<",  N00B_CG_OP_SHL},
    {">>",  N00B_CG_OP_SHR},
    {"==",  N00B_CG_OP_EQ},
    {"!=",  N00B_CG_OP_NE},
    {"<",   N00B_CG_OP_LT},
    {"<=",  N00B_CG_OP_LE},
    {">",   N00B_CG_OP_GT},
    {">=",  N00B_CG_OP_GE},
    {"!",   N00B_CG_OP_LOGICAL_NOT},
    {"&&",  N00B_CG_OP_LOGICAL_AND},
    {"||",  N00B_CG_OP_LOGICAL_OR},
    {"and", N00B_CG_OP_LOGICAL_AND},
    {"or",  N00B_CG_OP_LOGICAL_OR},
};

#define NUM_DEFAULT_OPS (sizeof(default_ops) / sizeof(default_ops[0]))

// ============================================================================
// Lifecycle
// ============================================================================

n00b_codegen_t *
n00b_codegen_new(n00b_grammar_t *grammar)
_kargs {
    n00b_annot_result_t *annot;
    n00b_cg_type_map_fn  type_map;
    n00b_cg_literal_fn   literal_parser;
    n00b_cg_storage_fn   storage_policy;
    n00b_cg_handler_fn   default_handler;
    void                *user_data;
    const char          *module_name;
}
{
    n00b_codegen_t *cg = n00b_alloc(n00b_codegen_t);

    cg->grammar        = grammar;
    cg->annot          = kargs->annot;
    cg->type_map       = kargs->type_map;
    cg->literal_parser = kargs->literal_parser;
    cg->storage_policy = kargs->storage_policy;
    cg->default_handler = kargs->default_handler;
    cg->user_data      = kargs->user_data;
    cg->diag           = n00b_diag_ctx_new();

    const char *mod_name = kargs->module_name ? kargs->module_name : "main";

    // Initialize MIR.
    cg->mir_ctx    = MIR_init();
    cg->mir_module = MIR_new_module(cg->mir_ctx, mod_name);
    cg->linked     = false;

    // Allocate handler table if we have a grammar.
    if (grammar) {
        int64_t nt_count = (int64_t)n00b_list_len(grammar->nt_list);

        if (nt_count > 0) {
            cg->handler_cap = (int32_t)nt_count;
            cg->handlers    = n00b_alloc_array(
                n00b_cg_handler_fn, (size_t)nt_count);
        }
    }

    // Install default operator map.
    cg->op_map_cap   = (int32_t)(NUM_DEFAULT_OPS + 16);
    cg->op_map_count = (int32_t)NUM_DEFAULT_OPS;
    cg->op_map       = n00b_alloc_array(
        n00b_cg_op_entry_t, (size_t)cg->op_map_cap);

    for (int32_t i = 0; i < (int32_t)NUM_DEFAULT_OPS; i++) {
        cg->op_map[i].text = default_ops[i].text;
        cg->op_map[i].op   = default_ops[i].op;
    }

    return cg;
}

void
n00b_codegen_free(n00b_codegen_t *cg)
{
    if (!cg) {
        return;
    }

    if (cg->mir_ctx) {
        if (!cg->linked) {
            // Module was never finished/linked — finish it now so MIR
            // can clean up properly.
            MIR_finish_module(cg->mir_ctx);
        }
        if (cg->gen_inited) {
            MIR_gen_finish(cg->mir_ctx);
        }
        MIR_finish(cg->mir_ctx);
    }

    if (cg->diag) {
        n00b_diag_ctx_free(cg->diag);
    }
}

// ============================================================================
// Handler registration
// ============================================================================

bool
n00b_codegen_register(n00b_codegen_t    *cg,
                       const char         *nt_name,
                       n00b_cg_handler_fn  handler)
{
    if (!cg->grammar) {
        return false;
    }

    n00b_string_t  name     = n00b_string_from_cstr(nt_name);
    n00b_string_t *name_ptr = &name;
    bool           found;
    int64_t        id = n00b_dict_get(cg->grammar->nt_map, name_ptr, &found);

    if (!found) {
        return false;
    }

    return n00b_codegen_register_by_id(cg, id, handler);
}

bool
n00b_codegen_register_by_id(n00b_codegen_t    *cg,
                              int64_t             nt_id,
                              n00b_cg_handler_fn  handler)
{
    if (!cg->handlers || nt_id < 0 || nt_id >= cg->handler_cap) {
        return false;
    }

    cg->handlers[nt_id] = handler;
    return true;
}

void
n00b_codegen_map_operator(n00b_codegen_t      *cg,
                           const char           *token_text,
                           n00b_cg_semantic_op_t op)
{
    // Check for existing entry.
    for (int32_t i = 0; i < cg->op_map_count; i++) {
        if (strcmp(cg->op_map[i].text, token_text) == 0) {
            cg->op_map[i].op = op;
            return;
        }
    }

    // Add new entry.
    if (cg->op_map_count >= cg->op_map_cap) {
        int32_t new_cap = cg->op_map_cap * 2;
        n00b_cg_op_entry_t *new_map = n00b_alloc_array(
            n00b_cg_op_entry_t, (size_t)new_cap);
        memcpy(new_map, cg->op_map,
               sizeof(n00b_cg_op_entry_t) * (size_t)cg->op_map_count);
        cg->op_map     = new_map;
        cg->op_map_cap = new_cap;
    }

    cg->op_map[cg->op_map_count++] = (n00b_cg_op_entry_t){
        .text = token_text,
        .op   = op,
    };
}

// ============================================================================
// Queries
// ============================================================================

n00b_cg_type_tag_t
n00b_codegen_node_type(n00b_codegen_t *cg, n00b_parse_tree_t *node)
{
    if (!cg->annot || !cg->annot->node_types) {
        return N00B_CG_I64;
    }

    bool            found;
    uintptr_t       key  = (uintptr_t)node;
    n00b_tc_type_t *type = n00b_dict_get(cg->annot->node_types,
                                           key, &found);

    if (!found || !type) {
        return N00B_CG_I64;
    }

    if (cg->type_map) {
        return cg->type_map(cg, type);
    }

    return N00B_CG_I64;
}

n00b_cf_label_t *
n00b_codegen_cf_label(n00b_codegen_t *cg, n00b_parse_tree_t *node)
{
    if (!cg->annot) {
        return NULL;
    }

    return n00b_cf_label_lookup(cg->annot->cf_labels, node);
}

n00b_grammar_t *
n00b_codegen_grammar(n00b_codegen_t *cg)
{
    return cg->grammar;
}

n00b_annot_result_t *
n00b_codegen_annot(n00b_codegen_t *cg)
{
    return cg->annot;
}

n00b_diag_ctx_t *
n00b_codegen_diagnostics(n00b_codegen_t *cg)
{
    return cg->diag;
}

void *
n00b_codegen_get_user_data(n00b_codegen_t *cg)
{
    return cg->user_data;
}

// ============================================================================
// Default literal parser (strtoll / strtod)
// ============================================================================

static n00b_cg_val_t
default_literal_parser(n00b_codegen_t    *cg,
                       n00b_parse_tree_t *node,
                       n00b_string_t      lit_kind,
                       n00b_cg_type_tag_t type_tag)
{
    const char *text = n00b_pt_token_text(node);
    size_t      len  = n00b_pt_token_text_len(node);

    if (!text || len == 0) {
        return _n00b_cg_const_i64(cg, 0);
    }

    // Make a null-terminated copy.
    char *buf = n00b_alloc_size(1, len + 1);
    memcpy(buf, text, len);
    buf[len] = '\0';

    if (type_tag == N00B_CG_F64) {
        return _n00b_cg_const_f64(cg, strtod(buf, NULL));
    }

    if (type_tag == N00B_CG_F32) {
        return _n00b_cg_const_f32(cg, strtof(buf, NULL));
    }

    return _n00b_cg_const_i64(cg, strtoll(buf, NULL, 0));
}

// ============================================================================
// Tree walk helpers (forward declarations)
// ============================================================================

static n00b_cg_val_t codegen_walk(n00b_codegen_t *cg, n00b_parse_tree_t *node);
static n00b_cg_val_t codegen_operator(n00b_codegen_t *cg, n00b_parse_tree_t *node);
static n00b_cg_val_t codegen_literal(n00b_codegen_t *cg, n00b_parse_tree_t *node,
                                      n00b_annotation_t *annot);
static n00b_cg_val_t codegen_call_auto(n00b_codegen_t *cg, n00b_parse_tree_t *node,
                                        n00b_annotation_t *annot);
static n00b_cg_val_t codegen_branch(n00b_codegen_t *cg, n00b_cf_label_t *cf);
static n00b_cg_val_t codegen_loop(n00b_codegen_t *cg, n00b_cf_label_t *cf);
static n00b_cg_val_t codegen_jump(n00b_codegen_t *cg, n00b_cf_label_t *cf);
static n00b_cg_val_t codegen_assign(n00b_codegen_t *cg, n00b_cf_label_t *cf);
static n00b_cg_val_t codegen_varref(n00b_codegen_t *cg, n00b_cf_label_t *cf);
static n00b_cg_val_t codegen_children_default(n00b_codegen_t *cg,
                                               n00b_parse_tree_t *node);

// ============================================================================
// Short-circuit logical operators
// ============================================================================

static n00b_cg_val_t
codegen_short_circuit(n00b_codegen_t       *cg,
                      n00b_cg_semantic_op_t op,
                      n00b_parse_tree_t    *left,
                      n00b_parse_tree_t    *right)
{
    n00b_cg_val_t result      = n00b_cg_temp(cg, N00B_CG_I64);
    n00b_cg_val_t short_label = n00b_cg_label_new(cg);
    n00b_cg_val_t end_label   = n00b_cg_label_new(cg);

    n00b_cg_val_t left_val = codegen_walk(cg, left);

    if (op == N00B_CG_OP_LOGICAL_AND) {
        // AND: short-circuit to false if left is false.
        n00b_cg_emit_bf(cg, left_val, short_label);
    } else {
        // OR: short-circuit to true if left is true.
        n00b_cg_emit_bt(cg, left_val, short_label);
    }

    // Evaluate right side.
    n00b_cg_val_t right_val = codegen_walk(cg, right);
    n00b_cg_store(cg, result, right_val);
    n00b_cg_emit_jmp(cg, end_label);

    // Short-circuit label.
    n00b_cg_label_here(cg, short_label);

    if (op == N00B_CG_OP_LOGICAL_AND) {
        n00b_cg_store(cg, result, _n00b_cg_const_i64(cg, 0));
    } else {
        n00b_cg_store(cg, result, _n00b_cg_const_i64(cg, 1));
    }

    n00b_cg_label_here(cg, end_label);

    return result;
}

// ============================================================================
// Operator codegen
// ============================================================================

static n00b_cg_val_t
codegen_operator(n00b_codegen_t *cg, n00b_parse_tree_t *node)
{
    size_t n_children = n00b_pt_num_children(node);

    // Find the operator token.
    const char *op_text = NULL;

    for (size_t i = 0; i < n_children; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(node, i);

        if (n00b_pt_is_token(child)) {
            op_text = n00b_pt_token_text(child);
            break;
        }
    }

    if (!op_text) {
        return codegen_children_default(cg, node);
    }

    // Make a null-terminated copy for lookup.
    size_t op_len = strlen(op_text); // token text is in string pool, may be NUL-terminated
    char   op_buf[32];

    if (op_len >= sizeof(op_buf)) {
        op_len = sizeof(op_buf) - 1;
    }

    memcpy(op_buf, op_text, op_len);
    op_buf[op_len] = '\0';

    int32_t sem_op = n00b_cg_lookup_op(cg, op_buf);

    if (sem_op < 0) {
        return codegen_children_default(cg, node);
    }

    // Determine binary vs unary from child structure.
    // Collect non-token children (NT children are operands).
    n00b_parse_tree_t *operands[8];
    int                n_operands = 0;

    for (size_t i = 0; i < n_children && n_operands < 8; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(node, i);

        if (!n00b_pt_is_token(child)) {
            operands[n_operands++] = child;
        }
    }

    if (n_operands == 2) {
        // Check for short-circuit.
        if (sem_op == N00B_CG_OP_LOGICAL_AND || sem_op == N00B_CG_OP_LOGICAL_OR) {
            return codegen_short_circuit(cg, (n00b_cg_semantic_op_t)sem_op,
                                         operands[0], operands[1]);
        }

        n00b_cg_val_t a = codegen_walk(cg, operands[0]);
        n00b_cg_val_t b = codegen_walk(cg, operands[1]);

        return n00b_cg_emit_binop(cg, (n00b_cg_semantic_op_t)sem_op, a, b);
    }

    if (n_operands == 1) {
        n00b_cg_val_t a = codegen_walk(cg, operands[0]);
        return n00b_cg_emit_unop(cg, (n00b_cg_semantic_op_t)sem_op, a);
    }

    return codegen_children_default(cg, node);
}

// ============================================================================
// Literal codegen
// ============================================================================

static n00b_cg_val_t
codegen_literal(n00b_codegen_t    *cg,
                n00b_parse_tree_t *node,
                n00b_annotation_t *annot)
{
    n00b_cg_type_tag_t type = n00b_codegen_node_type(cg, node);

    // Find the token leaf for the literal value.
    n00b_parse_tree_t *tok_node = n00b_pt_first_token(node);

    if (!tok_node) {
        return _n00b_cg_const_i64(cg, 0);
    }

    n00b_string_t lit_kind = annot->op_kind;

    if (cg->literal_parser) {
        return cg->literal_parser(cg, tok_node, lit_kind, type);
    }

    return default_literal_parser(cg, tok_node, lit_kind, type);
}

// ============================================================================
// Auto-inferred call codegen
// ============================================================================

static n00b_cg_val_t
codegen_call_auto(n00b_codegen_t    *cg,
                  n00b_parse_tree_t *node,
                  n00b_annotation_t *annot)
{
    // @call(func_ref, args_ref)
    // Resolve the function name from the func_ref child.
    n00b_parse_tree_t *func_node = NULL;
    n00b_parse_tree_t *args_node = NULL;

    if (annot->name_ref.kind == N00B_ROLE_BY_INDEX) {
        int32_t idx = annot->name_ref.index;

        if (idx >= 0) {
            func_node = n00b_pt_get_child(node, (size_t)idx);
        }
    }

    // BNF parser stores @call($0, $2) second arg in type_ref.
    // Check value_ref first, then type_ref as fallback.
    if (annot->value_ref.kind == N00B_ROLE_BY_INDEX
        && annot->value_ref.index >= 0) {
        args_node = n00b_pt_get_child(node, (size_t)annot->value_ref.index);
    }
    else if (annot->type_ref.kind == N00B_ROLE_BY_INDEX
             && annot->type_ref.index >= 0) {
        args_node = n00b_pt_get_child(node, (size_t)annot->type_ref.index);
    }

    const char *func_name = NULL;

    if (func_node) {
        n00b_parse_tree_t *name_tok = n00b_pt_first_token(func_node);

        if (name_tok) {
            func_name = n00b_pt_token_text(name_tok);
        }
    }

    if (!func_name) {
        return codegen_children_default(cg, node);
    }

    // Collect arguments.
    n00b_cg_val_t args[32];
    int32_t       n_args = 0;

    if (args_node) {
        size_t nc = n00b_pt_num_children(args_node);

        for (size_t i = 0; i < nc && n_args < 32; i++) {
            n00b_parse_tree_t *arg = n00b_pt_get_child(args_node, i);

            if (!n00b_pt_is_token(arg) && !n00b_pt_is_group(arg)) {
                args[n_args++] = codegen_walk(cg, arg);
            } else if (n00b_pt_is_group(arg)) {
                // Flatten group children.
                size_t gnc = n00b_pt_num_children(arg);

                for (size_t j = 0; j < gnc && n_args < 32; j++) {
                    n00b_parse_tree_t *gc = n00b_pt_get_child(arg, j);

                    if (!n00b_pt_is_token(gc)) {
                        args[n_args++] = codegen_walk(cg, gc);
                    }
                }
            }
        }
    }

    n00b_cg_type_tag_t ret_type = n00b_codegen_node_type(cg, node);

    return n00b_cg_emit_call(cg, func_name, args, n_args,
                              .ret = ret_type);
}

// ============================================================================
// Control flow codegen
// ============================================================================

static n00b_cg_val_t
codegen_branch(n00b_codegen_t *cg, n00b_cf_label_t *cf)
{
    n00b_cg_val_t else_label = n00b_cg_label_new(cg);
    n00b_cg_val_t end_label  = n00b_cg_label_new(cg);

    // Evaluate condition.
    n00b_cg_val_t cond = codegen_walk(cg, cf->cond);

    n00b_cg_emit_bf(cg, cond, else_label);

    // Then branch.
    if (cf->then_body) {
        codegen_walk(cg, cf->then_body);
    }

    n00b_cg_emit_jmp(cg, end_label);

    // Else branch.
    n00b_cg_label_here(cg, else_label);

    if (cf->else_body) {
        codegen_walk(cg, cf->else_body);
    }

    n00b_cg_label_here(cg, end_label);

    return N00B_CG_VOID_VAL;
}

static n00b_cg_val_t
codegen_loop(n00b_codegen_t *cg, n00b_cf_label_t *cf)
{
    n00b_cg_val_t continue_label = n00b_cg_label_new(cg);
    n00b_cg_val_t break_label    = n00b_cg_label_new(cg);

    // Push loop stack for break/continue.
    if (cg->loop_depth >= cg->loop_cap) {
        int32_t new_cap = cg->loop_cap ? cg->loop_cap * 2 : 8;
        n00b_cg_loop_entry_t *new_stack = n00b_alloc_array(
            n00b_cg_loop_entry_t, (size_t)new_cap);
        if (cg->loop_stack) {
            memcpy(new_stack, cg->loop_stack,
                   sizeof(n00b_cg_loop_entry_t) * (size_t)cg->loop_depth);
        }
        cg->loop_stack = new_stack;
        cg->loop_cap   = new_cap;
    }

    cg->loop_stack[cg->loop_depth++] = (n00b_cg_loop_entry_t){
        .break_label    = break_label,
        .continue_label = continue_label,
    };

    // Loop top.
    n00b_cg_label_here(cg, continue_label);

    // Condition.
    if (cf->cond) {
        n00b_cg_val_t cond = codegen_walk(cg, cf->cond);
        n00b_cg_emit_bf(cg, cond, break_label);
    }

    // Body.
    if (cf->then_body) {
        codegen_walk(cg, cf->then_body);
    }

    n00b_cg_emit_jmp(cg, continue_label);

    // Exit.
    n00b_cg_label_here(cg, break_label);

    cg->loop_depth--;

    return N00B_CG_VOID_VAL;
}

static n00b_cg_val_t
codegen_jump(n00b_codegen_t *cg, n00b_cf_label_t *cf)
{
    const char *jk = cf->jump_kind.data;

    if (!jk) {
        return N00B_CG_VOID_VAL;
    }

    if (strncmp(jk, "return", 6) == 0) {
        // Return: if there's a value child in self, lower it.
        size_t nc = n00b_pt_num_children(cf->self);

        for (size_t i = 0; i < nc; i++) {
            n00b_parse_tree_t *child = n00b_pt_get_child(cf->self, i);

            if (!n00b_pt_is_token(child)) {
                n00b_cg_val_t val = codegen_walk(cg, child);
                n00b_cg_emit_ret(cg, val);
                return N00B_CG_VOID_VAL;
            }
        }

        n00b_cg_emit_ret_void(cg);
        return N00B_CG_VOID_VAL;
    }

    if (strncmp(jk, "break", 5) == 0) {
        if (cg->loop_depth > 0) {
            n00b_cg_emit_jmp(cg, cg->loop_stack[cg->loop_depth - 1].break_label);
        }
        return N00B_CG_VOID_VAL;
    }

    if (strncmp(jk, "continue", 8) == 0) {
        if (cg->loop_depth > 0) {
            n00b_cg_emit_jmp(cg, cg->loop_stack[cg->loop_depth - 1].continue_label);
        }
        return N00B_CG_VOID_VAL;
    }

    return N00B_CG_VOID_VAL;
}

static n00b_cg_val_t
codegen_assign(n00b_codegen_t *cg, n00b_cf_label_t *cf)
{
    // @assigns(name_ref, value_ref)
    // Lower the value.
    n00b_cg_val_t value = N00B_CG_VOID_VAL;

    if (cf->then_body) {
        value = codegen_walk(cg, cf->then_body);
    }

    // Find the variable name from cond (which holds the name subtree).
    if (cf->cond) {
        n00b_parse_tree_t *name_tok = n00b_pt_first_token(cf->cond);

        if (name_tok) {
            const char *var_name = n00b_pt_token_text(name_tok);

            if (var_name) {
                // Look up or create a local for this variable.
                // For now, create a temp and store into it.
                n00b_cg_val_t dst = n00b_cg_local(cg, var_name,
                                                     .type = value.type_tag);
                n00b_cg_store(cg, dst, value);
                return dst;
            }
        }
    }

    return value;
}

static n00b_cg_val_t
codegen_varref(n00b_codegen_t *cg, n00b_cf_label_t *cf)
{
    // @varref(name_ref) — look up the variable and return its value.
    if (cf->cond) {
        n00b_parse_tree_t *name_tok = n00b_pt_first_token(cf->cond);

        if (name_tok) {
            const char *var_name = n00b_pt_token_text(name_tok);

            if (var_name && cg->cur_func) {
                // Try to find an existing register with this name.
                MIR_func_t func = cg->cur_func->u.func;
                size_t     len  = n00b_pt_token_text_len(name_tok);
                char       buf[128];

                if (len >= sizeof(buf)) {
                    len = sizeof(buf) - 1;
                }

                memcpy(buf, var_name, len);
                buf[len] = '\0';

                MIR_reg_t reg = MIR_reg(cg->mir_ctx, buf, func);

                return (n00b_cg_val_t){
                    .id       = (uint32_t)reg,
                    .kind     = N00B_CG_VAL_REG,
                    .type_tag = n00b_codegen_node_type(cg, cf->self),
                };
            }
        }
    }

    return N00B_CG_VOID_VAL;
}

// ============================================================================
// Default: lower all children
// ============================================================================

static n00b_cg_val_t
codegen_children_default(n00b_codegen_t *cg, n00b_parse_tree_t *node)
{
    n00b_cg_val_t result = N00B_CG_VOID_VAL;
    size_t        nc     = n00b_pt_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(node, i);
        result = codegen_walk(cg, child);
    }

    return result;
}

// ============================================================================
// Core tree walk
// ============================================================================

static bool
annot_is_semantic(n00b_annot_kind_t kind)
{
    switch (kind) {
    case N00B_ANNOT_OPERATOR:
    case N00B_ANNOT_LITERAL:
    case N00B_ANNOT_CALL:
    case N00B_ANNOT_ASSIGNS:
    case N00B_ANNOT_VARREF:
    case N00B_ANNOT_BRANCH:
    case N00B_ANNOT_LOOP:
    case N00B_ANNOT_SWITCH:
    case N00B_ANNOT_JUMP:
    case N00B_ANNOT_CAPTURE:
        return true;
    default:
        return false;
    }
}

static bool
annot_is_format_only(n00b_annot_kind_t kind)
{
    switch (kind) {
    case N00B_ANNOT_INDENT:
    case N00B_ANNOT_NEWLINE:
    case N00B_ANNOT_SPACE:
    case N00B_ANNOT_NOSPACE:
    case N00B_ANNOT_GROUP:
    case N00B_ANNOT_SOFTLINE:
    case N00B_ANNOT_HARDLINE:
    case N00B_ANNOT_ALIGN:
    case N00B_ANNOT_CONCAT:
    case N00B_ANNOT_BLANKLINE:
    case N00B_ANNOT_PENALTY:
    case N00B_ANNOT_NOTRIVIA:
        return true;
    default:
        return false;
    }
}

static n00b_cg_val_t
codegen_walk(n00b_codegen_t *cg, n00b_parse_tree_t *node)
{
    if (!node) {
        return N00B_CG_VOID_VAL;
    }

    // Leaf token: return as immediate.
    if (n00b_parse_node_is_token(node)) {
        n00b_cg_type_tag_t type = n00b_codegen_node_type(cg, node);
        n00b_parse_tree_t *tok  = node;

        if (cg->literal_parser) {
            n00b_string_t no_kind = {0};
            return cg->literal_parser(cg, tok, no_kind, type);
        }

        return default_literal_parser(cg, tok, (n00b_string_t){0}, type);
    }

    n00b_nt_node_t *pn = &n00b_tree_node_value(node);

    // Skip group nodes: recurse through them.
    if (pn->group_top) {
        return codegen_children_default(cg, node);
    }

    // Check for explicit handler.
    if (cg->handlers && pn->id >= 0 && pn->id < cg->handler_cap
        && cg->handlers[pn->id]) {
        return cg->handlers[pn->id](cg, node);
    }

    // Check CF labels.
    n00b_cf_label_t *cf = n00b_codegen_cf_label(cg, node);

    if (cf) {
        switch (cf->kind) {
        case N00B_CF_BRANCH:        return codegen_branch(cg, cf);
        case N00B_CF_LOOP:          return codegen_loop(cg, cf);
        case N00B_CF_JUMP:          return codegen_jump(cg, cf);
        case N00B_CF_ASSIGNS:       return codegen_assign(cg, cf);
        case N00B_CF_VARREF:        return codegen_varref(cg, cf);
        case N00B_CF_SWITCH:        // Fall through to annotation check.
        case N00B_CF_CAPTURE:
        case N00B_CF_UNWRAP_RESULT:
            break;
        }
    }

    // Check rule annotations for semantic info.
    if (cg->grammar && pn->id >= 0) {
        n00b_parse_rule_t *rule = n00b_get_node_rule(cg->grammar, pn);

        if (rule && n00b_list_len(rule->annotations) > 0) {
            size_t n_annots = n00b_list_len(rule->annotations);

            for (size_t i = 0; i < n_annots; i++) {
                n00b_annotation_t *a = n00b_list_get(rule->annotations, i);

                if (!a) {
                    continue;
                }

                switch (a->kind) {
                case N00B_ANNOT_OPERATOR:
                    return codegen_operator(cg, node);
                case N00B_ANNOT_LITERAL:
                    return codegen_literal(cg, node, a);
                case N00B_ANNOT_CALL:
                    return codegen_call_auto(cg, node, a);
                default:
                    break;
                }
            }
        }

        // Also check NT-level pending annotations.
        n00b_nonterm_t *nt = n00b_get_nonterm(cg->grammar, pn->id);

        if (nt && n00b_list_len(nt->pending_annotations) > 0) {
            size_t n_annots = n00b_list_len(nt->pending_annotations);

            for (size_t i = 0; i < n_annots; i++) {
                n00b_annotation_t *a = n00b_list_get(
                    nt->pending_annotations, i);

                if (!a) {
                    continue;
                }

                switch (a->kind) {
                case N00B_ANNOT_OPERATOR:
                    return codegen_operator(cg, node);
                case N00B_ANNOT_LITERAL:
                    return codegen_literal(cg, node, a);
                case N00B_ANNOT_CALL:
                    return codegen_call_auto(cg, node, a);
                default:
                    break;
                }
            }
        }
    }

    // Auto-detect operator nodes: if the node has exactly one token
    // child whose text is a known operator and 1-2 NT children, treat
    // it as a binary/unary operator expression.  This lets the grammar
    // omit explicit @operator annotations on every precedence rule.
    {
        size_t nc          = n00b_pt_num_children(node);
        int    n_tok       = 0;
        int    n_nt        = 0;
        bool   has_known_op = false;

        for (size_t i = 0; i < nc; i++) {
            n00b_parse_tree_t *child = n00b_pt_get_child(node, i);

            if (n00b_pt_is_token(child)) {
                n_tok++;

                if (n_tok == 1) {
                    const char *text = n00b_pt_token_text(child);

                    if (text && n00b_cg_lookup_op(cg, text) >= 0) {
                        has_known_op = true;
                    }
                }
            }
            else if (!n00b_pt_is_group(child)) {
                n_nt++;
            }
        }

        if (has_known_op && n_tok == 1 && (n_nt == 1 || n_nt == 2)) {
            return codegen_operator(cg, node);
        }
    }

    // Default handler.
    if (cg->default_handler) {
        return cg->default_handler(cg, node);
    }

    // Fall through: lower all children.
    return codegen_children_default(cg, node);
}

// ============================================================================
// Public emission entry point
// ============================================================================

n00b_cg_val_t
n00b_codegen_lower(n00b_codegen_t *cg, n00b_parse_tree_t *child)
{
    return codegen_walk(cg, child);
}

bool
n00b_codegen_emit(n00b_codegen_t *cg, n00b_parse_tree_t *tree)
{
    if (!tree) {
        return false;
    }

    codegen_walk(cg, tree);

    return !n00b_diag_has_errors(cg->diag);
}

// ============================================================================
// Pre-flight audit
// ============================================================================

n00b_cg_audit_t
n00b_codegen_audit(n00b_codegen_t *cg)
{
    n00b_cg_audit_t result = {0};

    if (!cg->grammar) {
        return result;
    }

    int64_t nt_count = (int64_t)n00b_list_len(cg->grammar->nt_list);

    if (nt_count == 0) {
        return result;
    }

    // Temporary arrays for classification.
    const char **unhandled    = n00b_alloc_array(const char *, (size_t)nt_count);
    const char **auto_infer   = n00b_alloc_array(const char *, (size_t)nt_count);
    const char **explicit_arr = n00b_alloc_array(const char *, (size_t)nt_count);

    int32_t uh_count  = 0;
    int32_t ai_count  = 0;
    int32_t ex_count  = 0;

    for (int64_t id = 0; id < nt_count; id++) {
        n00b_nonterm_t *nt = n00b_get_nonterm(cg->grammar, id);

        if (!nt) {
            continue;
        }

        const char *name = nt->name.data;

        if (!name) {
            continue;
        }

        // Skip group NTs.
        if (nt->group_nt) {
            continue;
        }

        // Skip $$-prefixed internal NTs.
        if (name[0] == '$' && name[1] == '$') {
            continue;
        }

        // Check explicit handler.
        if (cg->handlers && id < cg->handler_cap && cg->handlers[id]) {
            explicit_arr[ex_count++] = name;
            continue;
        }

        // Check for semantic annotations.
        bool has_semantic = false;

        // Check pending annotations on the NT itself.
        if (n00b_list_len(nt->pending_annotations) > 0) {
            size_t na = n00b_list_len(nt->pending_annotations);

            for (size_t i = 0; i < na; i++) {
                n00b_annotation_t *a = n00b_list_get(
                    nt->pending_annotations, i);

                if (a && annot_is_semantic(a->kind)) {
                    has_semantic = true;
                    break;
                }
            }
        }

        // Check rule-level annotations.
        if (!has_semantic) {
            size_t n_rules = n00b_list_len(nt->rule_ids);

            for (size_t ri = 0; ri < n_rules && !has_semantic; ri++) {
                int32_t            gix  = n00b_list_get(nt->rule_ids, ri);
                n00b_parse_rule_t *rule = n00b_get_rule(cg->grammar, gix);

                if (!rule) {
                    continue;
                }

                size_t na = n00b_list_len(rule->annotations);

                for (size_t ai = 0; ai < na; ai++) {
                    n00b_annotation_t *a = n00b_list_get(rule->annotations, ai);

                    if (a && annot_is_semantic(a->kind)) {
                        has_semantic = true;
                        break;
                    }
                }
            }
        }

        if (has_semantic) {
            auto_infer[ai_count++] = name;
            continue;
        }

        // Check if this NT only has formatting annotations.
        bool only_format = true;

        if (n00b_list_len(nt->pending_annotations) > 0) {
            size_t na = n00b_list_len(nt->pending_annotations);

            for (size_t i = 0; i < na; i++) {
                n00b_annotation_t *a = n00b_list_get(
                    nt->pending_annotations, i);

                if (a && !annot_is_format_only(a->kind)
                    && a->kind != N00B_ANNOT_NONE) {
                    only_format = false;
                    break;
                }
            }
        }

        if (only_format && n00b_list_len(nt->pending_annotations) > 0) {
            // Skip NTs with only formatting annotations.
            continue;
        }

        unhandled[uh_count++] = name;
    }

    result.unhandled_nts     = unhandled;
    result.unhandled_count   = uh_count;
    result.auto_inferred     = auto_infer;
    result.auto_inferred_count = ai_count;
    result.explicit_handled  = explicit_arr;
    result.explicit_count    = ex_count;

    return result;
}

void
n00b_cg_audit_free(n00b_cg_audit_t *audit)
{
    // Arrays are n00b_alloc'd, will be GC'd.
    if (audit) {
        audit->unhandled_nts    = NULL;
        audit->auto_inferred    = NULL;
        audit->explicit_handled = NULL;
        audit->unhandled_count  = 0;
        audit->auto_inferred_count = 0;
        audit->explicit_count   = 0;
    }
}

// ============================================================================
// Execution modes
// ============================================================================

static void
ensure_linked(n00b_codegen_t *cg, bool for_gen)
{
    if (cg->linked) {
        return;
    }

    MIR_finish_module(cg->mir_ctx);
    MIR_load_module(cg->mir_ctx, cg->mir_module);

    // Resolve imports before linking (MIR requires this order).
    for (int32_t i = 0; i < cg->import_count; i++) {
        MIR_load_external(cg->mir_ctx, cg->imports[i].name,
                           cg->imports[i].addr);
    }

    if (for_gen) {
        MIR_gen_init(cg->mir_ctx);
        cg->gen_inited = true;
        MIR_link(cg->mir_ctx, MIR_set_gen_interface, NULL);
    } else {
        MIR_link(cg->mir_ctx, MIR_set_interp_interface, NULL);
    }

    cg->linked = true;
}

bool
n00b_codegen_interpret(n00b_codegen_t *cg,
                        const char      *func_name,
                        void            *result,
                        void            *args,
                        int32_t          n_args)
{
    ensure_linked(cg, false);

    MIR_item_t func = n00b_cg_find_func(cg, func_name);

    if (!func) {
        return false;
    }

    MIR_val_t res;
    MIR_val_t *arg_vals = (MIR_val_t *)args;

    if (n_args > 0 && arg_vals) {
        // MIR_interp takes varargs, so we need to call it based on count.
        // We use MIR_interp_arr for array-based calling.
        // Actually, MIR_interp is vararg-based. Let's use interp_arr.
        MIR_interp_arr(cg->mir_ctx, func, &res, (size_t)n_args, arg_vals);
    } else {
        MIR_interp(cg->mir_ctx, func, &res, 0);
    }

    if (result) {
        memcpy(result, &res, sizeof(MIR_val_t));
    }

    return true;
}

void *
n00b_codegen_jit(n00b_codegen_t *cg, const char *func_name)
{
    ensure_linked(cg, true);

    MIR_item_t func = n00b_cg_find_func(cg, func_name);

    if (!func) {
        return NULL;
    }

    return MIR_gen(cg->mir_ctx, func);
}

void
n00b_codegen_dump(n00b_codegen_t *cg, FILE *f)
{
    MIR_output(cg->mir_ctx, f);
}

// ============================================================================
// Convenience helpers
// ============================================================================

bool
n00b_cg_emit_func_from_tree(n00b_codegen_t    *cg,
                              n00b_parse_tree_t *tree,
                              const char         *func_name)
_kargs {
    n00b_cg_type_tag_t ret;
}
{
    if (!cg || !tree || !func_name) {
        return false;
    }

    n00b_cg_type_tag_t ret_type = kargs->ret; // default 0 = N00B_CG_I8

    // Treat unset (0) as I64 since that's the most common REPL use case.
    if (ret_type == 0) {
        ret_type = N00B_CG_I64;
    }

    n00b_cg_begin_func(cg, func_name, .ret = ret_type);

    n00b_cg_val_t result = n00b_codegen_lower(cg, tree);

    if (result.kind != N00B_CG_VAL_VOID) {
        n00b_cg_emit_ret(cg, result);
    }
    else if (ret_type == N00B_CG_VOID) {
        n00b_cg_emit_ret_void(cg);
    }
    else {
        // Return a zero value for the declared return type.
        n00b_cg_emit_ret(cg, _n00b_cg_const_i64(cg, 0));
    }

    n00b_cg_end_func(cg);

    return !n00b_diag_has_errors(cg->diag);
}

int64_t
n00b_codegen_eval_tree(n00b_grammar_t    *grammar,
                         n00b_parse_tree_t *tree)
_kargs {
    n00b_annot_result_t *annot;
    n00b_cg_type_map_fn  type_map;
    const char          *func_name;
    bool                *ok;
}
{
    if (kargs->ok) {
        *kargs->ok = false;
    }

    if (!tree) {
        return 0;
    }

    const char *fname = kargs->func_name ? kargs->func_name : "_eval";

    n00b_codegen_t *cg = n00b_codegen_new(grammar,
                                           .annot   = kargs->annot,
                                           .type_map = kargs->type_map);

    bool emit_ok = n00b_cg_emit_func_from_tree(cg, tree, fname,
                                                 .ret = N00B_CG_I64);

    if (!emit_ok) {
        n00b_codegen_free(cg);
        return 0;
    }

    typedef int64_t (*eval_fn_t)(void);

    eval_fn_t fn = (eval_fn_t)n00b_codegen_jit(cg, fname);

    if (!fn) {
        n00b_codegen_free(cg);
        return 0;
    }

    int64_t val = fn();

    if (kargs->ok) {
        *kargs->ok = true;
    }

    n00b_codegen_free(cg);

    return val;
}
