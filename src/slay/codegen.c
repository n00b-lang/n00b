/**
 * @file codegen.c
 * @brief Code generation engine: session/module lifecycle, tree walk, auto-inference, audit.
 *
 * This is the core codegen engine that walks a parse tree, dispatching
 * to registered handlers and auto-inferred codegen based on grammar
 * annotations (@operator, @literal, @branch, @loop, etc.).
 */

#include "n00b.h"
#include "internal/slay/codegen_internal.h"
#include "core/alloc.h"
#include "core/hash.h"

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
// Internal: install default operators on a session
// ============================================================================

static void
install_default_ops(n00b_cg_session_t *s)
{
    s->op_map_cap   = (int32_t)(NUM_DEFAULT_OPS + 16);
    s->op_map_count = (int32_t)NUM_DEFAULT_OPS;
    s->op_map       = n00b_alloc_array(
        n00b_cg_op_entry_t, (size_t)s->op_map_cap);

    for (int32_t i = 0; i < (int32_t)NUM_DEFAULT_OPS; i++) {
        s->op_map[i].text = default_ops[i].text;
        s->op_map[i].op   = default_ops[i].op;
    }
}

// ============================================================================
// Internal: install handler table from grammar
// ============================================================================

static void
install_handlers(n00b_cg_session_t *s)
{
    if (!s->grammar) {
        return;
    }

    int64_t nt_count = (int64_t)n00b_list_len(s->grammar->nt_list);

    if (nt_count > 0) {
        s->handler_cap = (int32_t)nt_count;
        s->handlers    = n00b_alloc_array(
            n00b_cg_handler_fn, (size_t)nt_count);
    }
}

// ============================================================================
// Session lifecycle
// ============================================================================

n00b_cg_session_t *
n00b_cg_session_new(n00b_grammar_t *grammar)
_kargs {
    n00b_cg_import_table_t *imports;
    n00b_cg_type_map_fn     type_map;
    n00b_cg_literal_fn      literal_parser;
    n00b_cg_storage_fn      storage_policy;
    n00b_cg_handler_fn      default_handler;
    void                   *user_data;
}
{
    n00b_cg_session_t *s = n00b_alloc(n00b_cg_session_t);

    s->grammar        = grammar;
    s->type_map       = kargs->type_map;
    s->literal_parser = kargs->literal_parser;
    s->storage_policy = kargs->storage_policy;
    s->default_handler = kargs->default_handler;
    s->user_data      = kargs->user_data;
    s->import_table   = kargs->imports;
    s->diag           = n00b_diag_ctx_new();
    s->global_scope   = n00b_symtab_new();

    // Module cache: C-string keyed dictionary for use-stmt loading.
    s->module_cache   = n00b_alloc(n00b_dict_untyped_t);
    n00b_dict_untyped_init(s->module_cache, .hash = n00b_hash_cstring);

    // Initialize MIR context (single, persistent).
    s->mir_ctx = MIR_init();

    install_handlers(s);
    install_default_ops(s);

    return s;
}

void
n00b_cg_session_free(n00b_cg_session_t *s)
{
    if (!s) {
        return;
    }

    if (s->mir_ctx) {
        // Finish any unfinished modules.
        for (int32_t i = 0; i < s->module_count; i++) {
            n00b_cg_module_t *m = s->modules[i];

            if (m && m->state == N00B_CG_MOD_BUILDING) {
                MIR_finish_module(s->mir_ctx);
                m->state = N00B_CG_MOD_FINISHED;
            }
        }

        if (s->gen_inited) {
            MIR_gen_finish(s->mir_ctx);
        }

        MIR_finish(s->mir_ctx);
    }

    if (s->diag) {
        n00b_diag_ctx_free(s->diag);
    }
}

// ============================================================================
// Backward-compatible lifecycle (old API)
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
    n00b_cg_session_t *s = n00b_cg_session_new(grammar,
        .type_map       = kargs->type_map,
        .literal_parser = kargs->literal_parser,
        .storage_policy = kargs->storage_policy,
        .default_handler = kargs->default_handler,
        .user_data      = kargs->user_data);

    // Store annotation result on session (old API pattern).
    s->annot = kargs->annot;

    // Create an initial module (old API always had one module).
    const char *mod_name = kargs->module_name ? kargs->module_name : "main";
    n00b_cg_module_new(s, mod_name);

    // Copy annot to the module too.
    if (s->active_module && kargs->annot) {
        s->active_module->annot = kargs->annot;
    }

    return s;
}

void
n00b_codegen_free(n00b_codegen_t *cg)
{
    n00b_cg_session_free(cg);
}

// ============================================================================
// Module lifecycle
// ============================================================================

n00b_cg_module_t *
n00b_cg_module_new(n00b_cg_session_t *s, const char *name)
{
    n00b_cg_module_t *m = n00b_alloc(n00b_cg_module_t);

    m->session    = s;
    m->name       = name;
    m->mir_module = MIR_new_module(s->mir_ctx, name);
    m->state      = N00B_CG_MOD_BUILDING;

    // Add to session's module list.
    if (s->module_count >= s->module_cap) {
        int32_t new_cap = s->module_cap ? s->module_cap * 2 : 8;
        n00b_cg_module_t **new_mods = n00b_alloc_array(
            n00b_cg_module_t *, (size_t)new_cap);

        if (s->modules) {
            memcpy(new_mods, s->modules,
                   sizeof(n00b_cg_module_t *) * (size_t)s->module_count);
        }

        s->modules    = new_mods;
        s->module_cap = new_cap;
    }

    s->modules[s->module_count++] = m;
    s->active_module = m;

    return m;
}

void *
n00b_cg_module_compile(n00b_cg_module_t *m, const char *entry_func)
{
    if (!m || !m->session) {
        return NULL;
    }

    n00b_cg_session_t *s = m->session;

    // Finish module if still building.
    if (m->state == N00B_CG_MOD_BUILDING) {
        MIR_finish_module(s->mir_ctx);
        m->state = N00B_CG_MOD_FINISHED;
    }

    // Load module.
    if (m->state == N00B_CG_MOD_FINISHED) {
        MIR_load_module(s->mir_ctx, m->mir_module);

        // Resolve per-module imports.
        for (int32_t i = 0; i < m->import_count; i++) {
            MIR_load_external(s->mir_ctx, m->imports[i].name,
                               m->imports[i].addr);
        }

        // Resolve session-level FFI imports.
        if (s->import_table) {
            for (int32_t i = 0; i < s->import_table->count; i++) {
                n00b_cg_import_entry_t *e = &s->import_table->entries[i];

                if (e->name && e->addr) {
                    MIR_load_external(s->mir_ctx, e->name, e->addr);
                }
            }
        }

        // Initialize generator if needed.
        if (!s->gen_inited) {
            MIR_gen_init(s->mir_ctx);
            s->gen_inited = true;
        }

        MIR_link(s->mir_ctx, MIR_set_gen_interface, NULL);
        m->state = N00B_CG_MOD_LINKED;
    }

    if (!entry_func) {
        m->state = N00B_CG_MOD_COMPILED;
        return NULL;
    }

    // Find the entry function in this module.
    MIR_item_t func = NULL;

    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, m->mir_module->items);
         item != NULL;
         item = DLIST_NEXT(MIR_item_t, item)) {
        if (item->item_type == MIR_func_item
            && strcmp(item->u.func->name, entry_func) == 0) {
            func = item;
            break;
        }
    }

    if (!func) {
        return NULL;
    }

    void *code = MIR_gen(s->mir_ctx, func);
    m->state = N00B_CG_MOD_COMPILED;

    return code;
}

void
n00b_cg_set_active_module(n00b_cg_session_t *s, n00b_cg_module_t *m)
{
    s->active_module = m;
}

// ============================================================================
// Handler registration
// ============================================================================

bool
n00b_codegen_register(n00b_cg_session_t  *s,
                       const char          *nt_name,
                       n00b_cg_handler_fn   handler)
{
    if (!s->grammar) {
        return false;
    }

    n00b_string_t  name     = n00b_string_from_cstr(nt_name);
    n00b_string_t *name_ptr = &name;
    bool           found;
    int64_t        id = n00b_dict_get(s->grammar->nt_map, name_ptr, &found);

    if (!found) {
        return false;
    }

    return n00b_codegen_register_by_id(s, id, handler);
}

bool
n00b_codegen_register_by_id(n00b_cg_session_t  *s,
                              int64_t              nt_id,
                              n00b_cg_handler_fn   handler)
{
    if (!s->handlers || nt_id < 0 || nt_id >= s->handler_cap) {
        return false;
    }

    s->handlers[nt_id] = handler;
    return true;
}

void
n00b_codegen_map_operator(n00b_cg_session_t     *s,
                           const char             *token_text,
                           n00b_cg_semantic_op_t   op)
{
    for (int32_t i = 0; i < s->op_map_count; i++) {
        if (strcmp(s->op_map[i].text, token_text) == 0) {
            s->op_map[i].op = op;
            return;
        }
    }

    if (s->op_map_count >= s->op_map_cap) {
        int32_t new_cap = s->op_map_cap * 2;
        n00b_cg_op_entry_t *new_map = n00b_alloc_array(
            n00b_cg_op_entry_t, (size_t)new_cap);
        memcpy(new_map, s->op_map,
               sizeof(n00b_cg_op_entry_t) * (size_t)s->op_map_count);
        s->op_map     = new_map;
        s->op_map_cap = new_cap;
    }

    s->op_map[s->op_map_count++] = (n00b_cg_op_entry_t){
        .text = token_text,
        .op   = op,
    };
}

// ============================================================================
// Queries
// ============================================================================

/**
 * @brief Get the current annotation result.
 *
 * Checks active module first, then session-level (backward compat).
 */
static inline n00b_annot_result_t *
current_annot(n00b_cg_session_t *s)
{
    if (s->active_module && s->active_module->annot) {
        return s->active_module->annot;
    }

    return s->annot;
}

n00b_cg_type_tag_t
n00b_codegen_node_type(n00b_cg_session_t *s, n00b_parse_tree_t *node)
{
    n00b_annot_result_t *a = current_annot(s);

    if (!a || !a->node_types) {
        return N00B_CG_I64;
    }

    bool            found;
    uintptr_t       key  = (uintptr_t)node;
    n00b_tc_type_t *type = n00b_dict_get(a->node_types, key, &found);

    if (!found || !type) {
        return N00B_CG_I64;
    }

    if (s->type_map) {
        return s->type_map(s, type);
    }

    return N00B_CG_I64;
}

n00b_cf_label_t *
n00b_codegen_cf_label(n00b_cg_session_t *s, n00b_parse_tree_t *node)
{
    n00b_annot_result_t *a = current_annot(s);

    if (!a) {
        return NULL;
    }

    return n00b_cf_label_lookup(a->cf_labels, node);
}

n00b_grammar_t *
n00b_codegen_grammar(n00b_cg_session_t *s)
{
    return s->grammar;
}

n00b_annot_result_t *
n00b_codegen_annot(n00b_cg_session_t *s)
{
    return current_annot(s);
}

n00b_diag_ctx_t *
n00b_codegen_diagnostics(n00b_cg_session_t *s)
{
    return s->diag;
}

void *
n00b_codegen_get_user_data(n00b_cg_session_t *s)
{
    return s->user_data;
}

// ============================================================================
// Default literal parser (strtoll / strtod)
// ============================================================================

static n00b_cg_val_t
default_literal_parser(n00b_cg_session_t  *s,
                       n00b_parse_tree_t  *node,
                       n00b_string_t       lit_kind,
                       n00b_cg_type_tag_t  type_tag)
{
    const char *text = n00b_pt_token_text(node);
    size_t      len  = n00b_pt_token_text_len(node);

    if (!text || len == 0) {
        return _n00b_cg_const_i64(s, 0);
    }

    char *buf = n00b_alloc_size(1, len + 1);
    memcpy(buf, text, len);
    buf[len] = '\0';

    if (type_tag == N00B_CG_F64) {
        return _n00b_cg_const_f64(s, strtod(buf, NULL));
    }

    if (type_tag == N00B_CG_F32) {
        return _n00b_cg_const_f32(s, strtof(buf, NULL));
    }

    return _n00b_cg_const_i64(s, strtoll(buf, NULL, 0));
}

// ============================================================================
// Tree walk helpers (forward declarations)
// ============================================================================

static n00b_cg_val_t codegen_walk(n00b_cg_session_t *s, n00b_parse_tree_t *node);
static n00b_cg_val_t codegen_operator(n00b_cg_session_t *s, n00b_parse_tree_t *node);
static n00b_cg_val_t codegen_literal(n00b_cg_session_t *s, n00b_parse_tree_t *node,
                                      n00b_annotation_t *annot);
static n00b_cg_val_t codegen_call_auto(n00b_cg_session_t *s, n00b_parse_tree_t *node,
                                        n00b_annotation_t *annot);
static n00b_cg_val_t codegen_branch(n00b_cg_session_t *s, n00b_cf_label_t *cf);
static n00b_cg_val_t codegen_loop(n00b_cg_session_t *s, n00b_cf_label_t *cf);
static n00b_cg_val_t codegen_jump(n00b_cg_session_t *s, n00b_cf_label_t *cf);
static n00b_cg_val_t codegen_assign(n00b_cg_session_t *s, n00b_cf_label_t *cf);
static n00b_cg_val_t codegen_varref(n00b_cg_session_t *s, n00b_cf_label_t *cf);
static n00b_cg_val_t codegen_children_default(n00b_cg_session_t *s,
                                               n00b_parse_tree_t *node);

// ============================================================================
// Short-circuit logical operators
// ============================================================================

static n00b_cg_val_t
codegen_short_circuit(n00b_cg_session_t    *s,
                      n00b_cg_semantic_op_t op,
                      n00b_parse_tree_t    *left,
                      n00b_parse_tree_t    *right)
{
    n00b_cg_val_t result      = n00b_cg_temp(s, N00B_CG_I64);
    n00b_cg_val_t short_label = n00b_cg_label_new(s);
    n00b_cg_val_t end_label   = n00b_cg_label_new(s);

    n00b_cg_val_t left_val = codegen_walk(s, left);

    if (op == N00B_CG_OP_LOGICAL_AND) {
        n00b_cg_emit_bf(s, left_val, short_label);
    } else {
        n00b_cg_emit_bt(s, left_val, short_label);
    }

    n00b_cg_val_t right_val = codegen_walk(s, right);
    n00b_cg_store(s, result, right_val);
    n00b_cg_emit_jmp(s, end_label);

    n00b_cg_label_here(s, short_label);

    if (op == N00B_CG_OP_LOGICAL_AND) {
        n00b_cg_store(s, result, _n00b_cg_const_i64(s, 0));
    } else {
        n00b_cg_store(s, result, _n00b_cg_const_i64(s, 1));
    }

    n00b_cg_label_here(s, end_label);

    return result;
}

// ============================================================================
// Operator codegen
// ============================================================================

static n00b_cg_val_t
codegen_operator(n00b_cg_session_t *s, n00b_parse_tree_t *node)
{
    size_t n_children = n00b_pt_num_children(node);

    const char *op_text = NULL;

    for (size_t i = 0; i < n_children; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(node, i);

        if (n00b_pt_is_token(child)) {
            op_text = n00b_pt_token_text(child);
            break;
        }
    }

    if (!op_text) {
        return codegen_children_default(s, node);
    }

    size_t op_len = strlen(op_text);
    char   op_buf[32];

    if (op_len >= sizeof(op_buf)) {
        op_len = sizeof(op_buf) - 1;
    }

    memcpy(op_buf, op_text, op_len);
    op_buf[op_len] = '\0';

    int32_t sem_op = n00b_cg_lookup_op(s, op_buf);

    if (sem_op < 0) {
        return codegen_children_default(s, node);
    }

    n00b_parse_tree_t *operands[8];
    int                n_operands = 0;

    for (size_t i = 0; i < n_children && n_operands < 8; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(node, i);

        if (!n00b_pt_is_token(child)) {
            operands[n_operands++] = child;
        }
    }

    if (n_operands == 2) {
        if (sem_op == N00B_CG_OP_LOGICAL_AND || sem_op == N00B_CG_OP_LOGICAL_OR) {
            return codegen_short_circuit(s, (n00b_cg_semantic_op_t)sem_op,
                                         operands[0], operands[1]);
        }

        n00b_cg_val_t a = codegen_walk(s, operands[0]);
        n00b_cg_val_t b = codegen_walk(s, operands[1]);

        return n00b_cg_emit_binop(s, (n00b_cg_semantic_op_t)sem_op, a, b);
    }

    if (n_operands == 1) {
        n00b_cg_val_t a = codegen_walk(s, operands[0]);
        return n00b_cg_emit_unop(s, (n00b_cg_semantic_op_t)sem_op, a);
    }

    return codegen_children_default(s, node);
}

// ============================================================================
// Literal codegen
// ============================================================================

static n00b_cg_val_t
codegen_literal(n00b_cg_session_t *s,
                n00b_parse_tree_t *node,
                n00b_annotation_t *annot)
{
    n00b_cg_type_tag_t type = n00b_codegen_node_type(s, node);

    n00b_parse_tree_t *tok_node = n00b_pt_first_token(node);

    if (!tok_node) {
        return _n00b_cg_const_i64(s, 0);
    }

    n00b_string_t lit_kind = annot->op_kind;

    if (s->literal_parser) {
        return s->literal_parser(s, tok_node, lit_kind, type);
    }

    return default_literal_parser(s, tok_node, lit_kind, type);
}

// ============================================================================
// Auto-inferred call codegen
// ============================================================================

static n00b_cg_val_t
codegen_call_auto(n00b_cg_session_t *s,
                  n00b_parse_tree_t *node,
                  n00b_annotation_t *annot)
{
    n00b_parse_tree_t *func_node = NULL;
    n00b_parse_tree_t *args_node = NULL;

    if (annot->name_ref.kind == N00B_ROLE_BY_INDEX) {
        int32_t idx = annot->name_ref.index;

        if (idx >= 0) {
            func_node = n00b_pt_get_child(node, (size_t)idx);
        }
    }

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
        return codegen_children_default(s, node);
    }

    n00b_cg_val_t args[32];
    int32_t       n_args = 0;

    if (args_node) {
        size_t nc = n00b_pt_num_children(args_node);

        for (size_t i = 0; i < nc && n_args < 32; i++) {
            n00b_parse_tree_t *arg = n00b_pt_get_child(args_node, i);

            if (!n00b_pt_is_token(arg) && !n00b_pt_is_group(arg)) {
                args[n_args++] = codegen_walk(s, arg);
            } else if (n00b_pt_is_group(arg)) {
                size_t gnc = n00b_pt_num_children(arg);

                for (size_t j = 0; j < gnc && n_args < 32; j++) {
                    n00b_parse_tree_t *gc = n00b_pt_get_child(arg, j);

                    if (!n00b_pt_is_token(gc)) {
                        args[n_args++] = codegen_walk(s, gc);
                    }
                }
            }
        }
    }

    n00b_cg_type_tag_t ret_type = n00b_codegen_node_type(s, node);

    return n00b_cg_emit_call(s, func_name, args, n_args,
                              .ret = ret_type);
}

// ============================================================================
// Control flow codegen
// ============================================================================

static n00b_cg_val_t
codegen_branch(n00b_cg_session_t *s, n00b_cf_label_t *cf)
{
    n00b_cg_val_t else_label = n00b_cg_label_new(s);
    n00b_cg_val_t end_label  = n00b_cg_label_new(s);

    n00b_cg_val_t cond = codegen_walk(s, cf->cond);

    n00b_cg_emit_bf(s, cond, else_label);

    if (cf->then_body) {
        codegen_walk(s, cf->then_body);
    }

    n00b_cg_emit_jmp(s, end_label);

    n00b_cg_label_here(s, else_label);

    if (cf->else_body) {
        codegen_walk(s, cf->else_body);
    }

    n00b_cg_label_here(s, end_label);

    return N00B_CG_VOID_VAL;
}

static n00b_cg_val_t
codegen_loop(n00b_cg_session_t *s, n00b_cf_label_t *cf)
{
    n00b_cg_module_t *m = s->active_module;

    n00b_cg_val_t continue_label = n00b_cg_label_new(s);
    n00b_cg_val_t break_label    = n00b_cg_label_new(s);

    if (m->loop_depth >= m->loop_cap) {
        int32_t new_cap = m->loop_cap ? m->loop_cap * 2 : 8;
        n00b_cg_loop_entry_t *new_stack = n00b_alloc_array(
            n00b_cg_loop_entry_t, (size_t)new_cap);
        if (m->loop_stack) {
            memcpy(new_stack, m->loop_stack,
                   sizeof(n00b_cg_loop_entry_t) * (size_t)m->loop_depth);
        }
        m->loop_stack = new_stack;
        m->loop_cap   = new_cap;
    }

    m->loop_stack[m->loop_depth++] = (n00b_cg_loop_entry_t){
        .break_label    = break_label,
        .continue_label = continue_label,
    };

    n00b_cg_label_here(s, continue_label);

    if (cf->cond) {
        n00b_cg_val_t cond = codegen_walk(s, cf->cond);
        n00b_cg_emit_bf(s, cond, break_label);
    }

    if (cf->then_body) {
        codegen_walk(s, cf->then_body);
    }

    n00b_cg_emit_jmp(s, continue_label);

    n00b_cg_label_here(s, break_label);

    m->loop_depth--;

    return N00B_CG_VOID_VAL;
}

static n00b_cg_val_t
codegen_jump(n00b_cg_session_t *s, n00b_cf_label_t *cf)
{
    n00b_cg_module_t *m = s->active_module;
    const char       *jk = cf->jump_kind.data;

    if (!jk) {
        return N00B_CG_VOID_VAL;
    }

    if (strncmp(jk, "return", 6) == 0) {
        size_t nc = n00b_pt_num_children(cf->self);

        for (size_t i = 0; i < nc; i++) {
            n00b_parse_tree_t *child = n00b_pt_get_child(cf->self, i);

            if (!n00b_pt_is_token(child)) {
                n00b_cg_val_t val = codegen_walk(s, child);
                n00b_cg_emit_ret(s, val);
                return N00B_CG_VOID_VAL;
            }
        }

        n00b_cg_emit_ret_void(s);
        return N00B_CG_VOID_VAL;
    }

    if (strncmp(jk, "break", 5) == 0) {
        if (m->loop_depth > 0) {
            n00b_cg_emit_jmp(s, m->loop_stack[m->loop_depth - 1].break_label);
        }
        return N00B_CG_VOID_VAL;
    }

    if (strncmp(jk, "continue", 8) == 0) {
        if (m->loop_depth > 0) {
            n00b_cg_emit_jmp(s, m->loop_stack[m->loop_depth - 1].continue_label);
        }
        return N00B_CG_VOID_VAL;
    }

    return N00B_CG_VOID_VAL;
}

static n00b_cg_val_t
codegen_assign(n00b_cg_session_t *s, n00b_cf_label_t *cf)
{
    n00b_cg_val_t value = N00B_CG_VOID_VAL;

    if (cf->then_body) {
        value = codegen_walk(s, cf->then_body);
    }

    if (cf->cond) {
        n00b_parse_tree_t *name_tok = n00b_pt_first_token(cf->cond);

        if (name_tok) {
            const char *var_name = n00b_pt_token_text(name_tok);

            if (var_name) {
                n00b_cg_val_t dst = n00b_cg_local(s, var_name,
                                                     .type = value.type_tag);
                n00b_cg_store(s, dst, value);
                return dst;
            }
        }
    }

    return value;
}

static n00b_cg_val_t
codegen_varref(n00b_cg_session_t *s, n00b_cf_label_t *cf)
{
    n00b_cg_module_t *m = s->active_module;

    if (cf->cond) {
        n00b_parse_tree_t *name_tok = n00b_pt_first_token(cf->cond);

        if (name_tok) {
            const char *var_name = n00b_pt_token_text(name_tok);

            if (var_name && m->cur_func) {
                MIR_func_t func = m->cur_func->u.func;
                size_t     len  = n00b_pt_token_text_len(name_tok);
                char       buf[128];

                if (len >= sizeof(buf)) {
                    len = sizeof(buf) - 1;
                }

                memcpy(buf, var_name, len);
                buf[len] = '\0';

                MIR_reg_t reg = MIR_reg(s->mir_ctx, buf, func);

                return (n00b_cg_val_t){
                    .id       = (uint32_t)reg,
                    .kind     = N00B_CG_VAL_REG,
                    .type_tag = n00b_codegen_node_type(s, cf->self),
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
codegen_children_default(n00b_cg_session_t *s, n00b_parse_tree_t *node)
{
    n00b_cg_val_t result = N00B_CG_VOID_VAL;
    size_t        nc     = n00b_pt_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(node, i);
        n00b_cg_val_t      val   = codegen_walk(s, child);

        if (val.kind != N00B_CG_VAL_VOID) {
            result = val;
        }
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
codegen_walk(n00b_cg_session_t *s, n00b_parse_tree_t *node)
{
    if (!node) {
        return N00B_CG_VOID_VAL;
    }

    // Leaf token: return as immediate.
    if (n00b_parse_node_is_token(node)) {
        n00b_cg_type_tag_t type = n00b_codegen_node_type(s, node);
        n00b_parse_tree_t *tok  = node;

        if (s->literal_parser) {
            n00b_string_t no_kind = {0};
            return s->literal_parser(s, tok, no_kind, type);
        }

        return default_literal_parser(s, tok, (n00b_string_t){0}, type);
    }

    n00b_nt_node_t *pn = &n00b_tree_node_value(node);

    // Skip group nodes: recurse through them.
    if (pn->group_top) {
        return codegen_children_default(s, node);
    }

    // Check for explicit handler.
    if (s->handlers && pn->id >= 0 && pn->id < s->handler_cap
        && s->handlers[pn->id]) {
        return s->handlers[pn->id](s, node);
    }

    // Check CF labels.
    n00b_cf_label_t *cf = n00b_codegen_cf_label(s, node);

    if (cf) {
        switch (cf->kind) {
        case N00B_CF_BRANCH:        return codegen_branch(s, cf);
        case N00B_CF_LOOP:          return codegen_loop(s, cf);
        case N00B_CF_JUMP:          return codegen_jump(s, cf);
        case N00B_CF_ASSIGNS:       return codegen_assign(s, cf);
        case N00B_CF_VARREF:        return codegen_varref(s, cf);
        case N00B_CF_SWITCH:
        case N00B_CF_CAPTURE:
        case N00B_CF_UNWRAP_RESULT:
            break;
        }
    }

    // Check rule annotations for semantic info.
    if (s->grammar && pn->id >= 0) {
        n00b_parse_rule_t *rule = n00b_get_node_rule(s->grammar, pn);

        if (rule && n00b_list_len(rule->annotations) > 0) {
            size_t n_annots = n00b_list_len(rule->annotations);

            for (size_t i = 0; i < n_annots; i++) {
                n00b_annotation_t *a = n00b_list_get(rule->annotations, i);

                if (!a) {
                    continue;
                }

                switch (a->kind) {
                case N00B_ANNOT_OPERATOR:
                    return codegen_operator(s, node);
                case N00B_ANNOT_LITERAL:
                    return codegen_literal(s, node, a);
                case N00B_ANNOT_CALL:
                    return codegen_call_auto(s, node, a);
                default:
                    break;
                }
            }
        }

        // Also check NT-level pending annotations.
        n00b_nonterm_t *nt = n00b_get_nonterm(s->grammar, pn->id);

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
                    return codegen_operator(s, node);
                case N00B_ANNOT_LITERAL:
                    return codegen_literal(s, node, a);
                case N00B_ANNOT_CALL:
                    return codegen_call_auto(s, node, a);
                default:
                    break;
                }
            }
        }
    }

    // Auto-detect operator nodes.
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
                    size_t      tlen = n00b_pt_token_text_len(child);

                    if (text && tlen > 0 && tlen < sizeof(char[32])) {
                        char buf[32];
                        memcpy(buf, text, tlen);
                        buf[tlen] = '\0';

                        if (n00b_cg_lookup_op(s, buf) >= 0) {
                            has_known_op = true;
                        }
                    }
                }
            }
            else if (!n00b_pt_is_group(child)) {
                n_nt++;
            }
        }

        if (has_known_op && n_tok == 1 && (n_nt == 1 || n_nt == 2)) {
            return codegen_operator(s, node);
        }
    }

    // Default handler.
    if (s->default_handler) {
        return s->default_handler(s, node);
    }

    // Fall through: lower all children.
    return codegen_children_default(s, node);
}

// ============================================================================
// Public emission entry point
// ============================================================================

n00b_cg_val_t
n00b_codegen_lower(n00b_cg_session_t *s, n00b_parse_tree_t *child)
{
    return codegen_walk(s, child);
}

bool
n00b_codegen_emit(n00b_cg_session_t *s, n00b_parse_tree_t *tree)
{
    if (!tree) {
        return false;
    }

    codegen_walk(s, tree);

    return !n00b_diag_has_errors(s->diag);
}

// ============================================================================
// Pre-flight audit
// ============================================================================

n00b_cg_audit_t
n00b_codegen_audit(n00b_cg_session_t *s)
{
    n00b_cg_audit_t result = {0};

    if (!s->grammar) {
        return result;
    }

    int64_t nt_count = (int64_t)n00b_list_len(s->grammar->nt_list);

    if (nt_count == 0) {
        return result;
    }

    const char **unhandled    = n00b_alloc_array(const char *, (size_t)nt_count);
    const char **auto_infer   = n00b_alloc_array(const char *, (size_t)nt_count);
    const char **explicit_arr = n00b_alloc_array(const char *, (size_t)nt_count);

    int32_t uh_count  = 0;
    int32_t ai_count  = 0;
    int32_t ex_count  = 0;

    for (int64_t id = 0; id < nt_count; id++) {
        n00b_nonterm_t *nt = n00b_get_nonterm(s->grammar, id);

        if (!nt) {
            continue;
        }

        const char *name = nt->name.data;

        if (!name) {
            continue;
        }

        if (nt->group_nt) {
            continue;
        }

        if (name[0] == '$' && name[1] == '$') {
            continue;
        }

        if (s->handlers && id < s->handler_cap && s->handlers[id]) {
            explicit_arr[ex_count++] = name;
            continue;
        }

        bool has_semantic = false;

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

        if (!has_semantic) {
            size_t n_rules = n00b_list_len(nt->rule_ids);

            for (size_t ri = 0; ri < n_rules && !has_semantic; ri++) {
                int32_t            gix  = n00b_list_get(nt->rule_ids, ri);
                n00b_parse_rule_t *rule = n00b_get_rule(s->grammar, gix);

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
            continue;
        }

        unhandled[uh_count++] = name;
    }

    result.unhandled_nts       = unhandled;
    result.unhandled_count     = uh_count;
    result.auto_inferred       = auto_infer;
    result.auto_inferred_count = ai_count;
    result.explicit_handled    = explicit_arr;
    result.explicit_count      = ex_count;

    return result;
}

void
n00b_cg_audit_free(n00b_cg_audit_t *audit)
{
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
// Execution modes (backward compat — operate on first module)
// ============================================================================

static void
ensure_linked(n00b_cg_session_t *s, bool for_gen)
{
    n00b_cg_module_t *m = s->active_module;

    if (!m) {
        return;
    }

    // Already linked or compiled — nothing to do.
    if (m->state >= N00B_CG_MOD_LINKED) {
        return;
    }

    if (m->state == N00B_CG_MOD_BUILDING) {
        MIR_finish_module(s->mir_ctx);
        m->state = N00B_CG_MOD_FINISHED;
    }

    MIR_load_module(s->mir_ctx, m->mir_module);

    // Resolve per-module imports.
    for (int32_t i = 0; i < m->import_count; i++) {
        MIR_load_external(s->mir_ctx, m->imports[i].name,
                           m->imports[i].addr);
    }

    if (for_gen) {
        if (!s->gen_inited) {
            MIR_gen_init(s->mir_ctx);
            s->gen_inited = true;
        }
        MIR_link(s->mir_ctx, MIR_set_gen_interface, NULL);
    } else {
        MIR_link(s->mir_ctx, MIR_set_interp_interface, NULL);
    }

    m->state = N00B_CG_MOD_LINKED;
}

bool
n00b_codegen_interpret(n00b_cg_session_t *s,
                        const char         *func_name,
                        void               *result,
                        void               *args,
                        int32_t             n_args)
{
    ensure_linked(s, false);

    MIR_item_t func = n00b_cg_find_func(s, func_name);

    if (!func) {
        return false;
    }

    MIR_val_t  res;
    MIR_val_t *arg_vals = (MIR_val_t *)args;

    if (n_args > 0 && arg_vals) {
        MIR_interp_arr(s->mir_ctx, func, &res, (size_t)n_args, arg_vals);
    } else {
        MIR_interp(s->mir_ctx, func, &res, 0);
    }

    if (result) {
        memcpy(result, &res, sizeof(MIR_val_t));
    }

    return true;
}

void *
n00b_codegen_jit(n00b_cg_session_t *s, const char *func_name)
{
    ensure_linked(s, true);

    MIR_item_t func = n00b_cg_find_func(s, func_name);

    if (!func) {
        return NULL;
    }

    return MIR_gen(s->mir_ctx, func);
}

void
n00b_codegen_dump(n00b_cg_session_t *s, FILE *f)
{
    MIR_output(s->mir_ctx, f);
}

void
n00b_cg_session_dump(n00b_cg_session_t *s, FILE *out)
{
    MIR_output(s->mir_ctx, out);
}

// ============================================================================
// Convenience helpers
// ============================================================================

bool
n00b_cg_emit_func_from_tree(n00b_cg_session_t *s,
                              n00b_parse_tree_t *tree,
                              const char         *func_name)
_kargs {
    n00b_cg_type_tag_t ret;
}
{
    if (!s || !tree || !func_name) {
        return false;
    }

    n00b_cg_type_tag_t ret_type = kargs->ret;

    // Treat unset (0) as I64 since that's the most common REPL use case.
    if (ret_type == 0) {
        ret_type = N00B_CG_I64;
    }

    n00b_cg_begin_func(s, func_name, .ret = ret_type);

    n00b_cg_val_t result = n00b_codegen_lower(s, tree);

    if (result.kind != N00B_CG_VAL_VOID) {
        n00b_cg_emit_ret(s, result);
    }
    else if (ret_type == N00B_CG_VOID) {
        n00b_cg_emit_ret_void(s);
    }
    else {
        n00b_cg_emit_ret(s, _n00b_cg_const_i64(s, 0));
    }

    n00b_cg_end_func(s);

    return !n00b_diag_has_errors(s->diag);
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
                                           .annot    = kargs->annot,
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

// ============================================================================
// Cross-module symbol resolution
// ============================================================================

void
n00b_cg_session_merge_module(n00b_cg_session_t *s, n00b_cg_module_t *m)
{
    if (!s || !m || !m->annot || !m->annot->symtab) {
        return;
    }

    n00b_symtab_t    *src = m->annot->symtab;
    n00b_namespace_t *ns  = n00b_symtab_ns(src, n00b_string_empty());

    if (!ns || !ns->all_scopes) {
        return;
    }

    // Walk top-level scopes (depth 0) looking for public functions.
    for (int32_t si = 0; si < ns->all_count; si++) {
        n00b_scope_t *scope = ns->all_scopes[si];

        if (!scope || scope->depth != 0) {
            continue;
        }

        for (n00b_sym_entry_t *sym = scope->first_in_scope; sym;
             sym                   = sym->next_in_scope) {
            if (sym->kind != N00B_SYM_FUNCTION) {
                continue;
            }

            n00b_symtab_add(s->global_scope, n00b_string_empty(),
                             sym->name, N00B_SYM_FUNCTION, sym->decl_node);
        }
    }
}

n00b_sym_entry_t *
n00b_cg_module_lookup(n00b_cg_module_t *m, const char *name)
{
    if (!m || !name) {
        return NULL;
    }

    // Try module's own symtab first.
    if (m->annot && m->annot->symtab) {
        n00b_string_t sname = n00b_string_from_cstr(name);
        n00b_sym_entry_t *sym = n00b_symtab_lookup_any(
            m->annot->symtab, n00b_string_empty(), sname);

        if (sym) {
            return sym;
        }
    }

    // Fall back to session global scope.
    if (m->session && m->session->global_scope) {
        n00b_string_t sname = n00b_string_from_cstr(name);

        return n00b_symtab_lookup_any(
            m->session->global_scope, n00b_string_empty(), sname);
    }

    return NULL;
}

n00b_cg_module_t *
n00b_cg_session_find_module(n00b_cg_session_t *s, const char *fqn)
{
    if (!s || !fqn || !s->module_cache) {
        return NULL;
    }

    bool found = false;
    void *val  = n00b_dict_untyped_get(s->module_cache, fqn, &found);

    return found ? (n00b_cg_module_t *)val : NULL;
}

// ============================================================================
// Session eval tree (new API)
// ============================================================================

int64_t
n00b_cg_session_eval_tree(n00b_cg_session_t *s,
                           n00b_parse_tree_t  *tree)
_kargs {
    n00b_annot_result_t *annot;
    const char          *func_name;
    bool                *ok;
}
{
    if (kargs->ok) {
        *kargs->ok = false;
    }

    if (!tree || !s) {
        return 0;
    }

    // Generate a unique module name.
    char mod_name[64];
    snprintf(mod_name, sizeof(mod_name), "repl_%d", s->module_count);

    const char *fname = kargs->func_name ? kargs->func_name : "_eval";

    // Create a new module for this expression.
    n00b_cg_module_t *m = n00b_cg_module_new(s, mod_name);

    // Set annotation result on the module.
    if (kargs->annot) {
        m->annot = kargs->annot;
    }

    // Emit the tree as a function.
    bool emit_ok = n00b_cg_emit_func_from_tree(s, tree, fname,
                                                 .ret = N00B_CG_I64);

    if (!emit_ok) {
        return 0;
    }

    // Compile the module and get entry function pointer.
    typedef int64_t (*eval_fn_t)(void);

    eval_fn_t fn = (eval_fn_t)n00b_cg_module_compile(m, fname);

    // Merge public symbols so later expressions can see them.
    n00b_cg_session_merge_module(s, m);

    if (!fn) {
        return 0;
    }

    int64_t val = fn();

    if (kargs->ok) {
        *kargs->ok = true;
    }

    return val;
}

// ============================================================================
// Import table management
// ============================================================================

n00b_cg_import_table_t *
n00b_cg_import_table_new(void)
{
    n00b_cg_import_table_t *t = n00b_alloc(n00b_cg_import_table_t);

    t->cap     = 32;
    t->count   = 0;
    t->entries = n00b_alloc_array(n00b_cg_import_entry_t, 32);

    return t;
}

void
n00b_cg_import_table_add(n00b_cg_import_table_t *table,
                           n00b_cg_import_entry_t  entry)
{
    if (table->count >= table->cap) {
        int32_t new_cap = table->cap * 2;
        n00b_cg_import_entry_t *new_entries = n00b_alloc_array(
            n00b_cg_import_entry_t, (size_t)new_cap);
        memcpy(new_entries, table->entries,
               sizeof(n00b_cg_import_entry_t) * (size_t)table->count);
        table->entries = new_entries;
        table->cap     = new_cap;
    }

    table->entries[table->count++] = entry;
}

void
n00b_cg_import_table_free(n00b_cg_import_table_t *table)
{
    // GC-allocated, nothing to do explicitly.
    (void)table;
}

void
n00b_cg_import_table_resolve_types(n00b_cg_import_table_t *table,
                                     n00b_tc_ctx_t           *ctx)
{
    if (!table || !ctx) {
        return;
    }

    for (int32_t i = 0; i < table->count; i++) {
        n00b_cg_import_entry_t *e = &table->entries[i];

        if (e->type_builder && !e->resolved_type) {
            e->resolved_type = e->type_builder(ctx);
        }
    }
}

// Platform-specific section boundary declarations.
#ifdef __APPLE__
extern const n00b_cg_import_entry_t __start_n00b_ffi
    __asm("section$start$__DATA$n00b_ffi");
extern const n00b_cg_import_entry_t __stop_n00b_ffi
    __asm("section$end$__DATA$n00b_ffi");
#else
extern const n00b_cg_import_entry_t __start_n00b_ffi;
extern const n00b_cg_import_entry_t __stop_n00b_ffi;
#endif

n00b_cg_import_table_t *
n00b_cg_collect_exports(void)
{
    const n00b_cg_import_entry_t *start = &__start_n00b_ffi;
    const n00b_cg_import_entry_t *stop  = &__stop_n00b_ffi;

    n00b_cg_import_table_t *table = n00b_cg_import_table_new();

    for (const n00b_cg_import_entry_t *e = start; e < stop; e++) {
        if (e->name && e->addr) {
            n00b_cg_import_table_add(table, *e);
        }
    }

    return table;
}
