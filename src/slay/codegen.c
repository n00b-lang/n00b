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
#include "n00b/n00b_compile_binary.h"
#include "n00b/embed.h"
#include "n00b/embed_ffi.h"
#include "slay/codegen_builtins.h"
#include "core/alloc.h"
#include "core/hash.h"
#include "core/type_info.h"
#include "typecheck/unify.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static FILE *
n00b_mir_open_code_len_trace(void)
{
    return tmpfile();
}

static void
n00b_mir_enable_code_len_trace(MIR_context_t ctx, FILE *trace)
{
#if !MIR_NO_GEN_DEBUG
    if (trace) {
        MIR_gen_set_debug_file(ctx, trace);
        MIR_gen_set_debug_level(ctx, 0);
    }
#else
    (void)ctx;
    (void)trace;
#endif
}

static bool
n00b_mir_finish_code_len_trace(MIR_context_t ctx,
                               FILE         *trace,
                               size_t       *code_lens,
                               size_t        func_count)
{
#if MIR_NO_GEN_DEBUG
    (void)ctx;
    (void)code_lens;
    (void)func_count;

    if (trace) {
        fclose(trace);
    }

    return false;
#else
    if (!trace) {
        return false;
    }

    fflush(trace);
    MIR_gen_set_debug_file(ctx, NULL);
    MIR_gen_set_debug_level(ctx, 100);

    rewind(trace);

    size_t found = 0;
    char   line[4096];

    while (fgets(line, sizeof(line), trace)) {
        char *p = strstr(line, " len=");

        if (!p) {
            continue;
        }

        p += 5;

        char              *end    = NULL;
        unsigned long long parsed = strtoull(p, &end, 10);

        if (end != p && *end == ')') {
            if (found < func_count) {
                code_lens[found] = (size_t)parsed;
            }

            found++;
        }
    }

    fclose(trace);

    return found == func_count;
#endif
}

// ============================================================================
// Default built-in operator table
// ============================================================================

static const struct {
    const char           *text;
    n00b_cg_semantic_op_t op;
} default_ops[] = {
    {"+", N00B_CG_OP_ADD},         {"-", N00B_CG_OP_SUB},
    {"*", N00B_CG_OP_MUL},         {"/", N00B_CG_OP_DIV},
    {"%", N00B_CG_OP_MOD},         {"&", N00B_CG_OP_AND},
    {"|", N00B_CG_OP_OR},          {"^", N00B_CG_OP_XOR},
    {"<<", N00B_CG_OP_SHL},        {">>", N00B_CG_OP_SHR},
    {"==", N00B_CG_OP_EQ},         {"!=", N00B_CG_OP_NE},
    {"<", N00B_CG_OP_LT},          {"<=", N00B_CG_OP_LE},
    {">", N00B_CG_OP_GT},          {">=", N00B_CG_OP_GE},
    {"!", N00B_CG_OP_LOGICAL_NOT}, {"&&", N00B_CG_OP_LOGICAL_AND},
    {"||", N00B_CG_OP_LOGICAL_OR}, {"and", N00B_CG_OP_LOGICAL_AND},
    {"or", N00B_CG_OP_LOGICAL_OR},
};

#define NUM_DEFAULT_OPS (sizeof(default_ops) / sizeof(default_ops[0]))

// ============================================================================
// Forward declarations for NT handlers registered during session init
// ============================================================================

static n00b_cg_val_t codegen_func_def(n00b_cg_session_t *s, n00b_parse_tree_t *node);
static n00b_cg_val_t codegen_assert_stmt(n00b_cg_session_t *s, n00b_parse_tree_t *node);
static n00b_cg_val_t codegen_comptime(n00b_cg_session_t *s, n00b_parse_tree_t *node);
static n00b_cg_val_t codegen_enum_stmt(n00b_cg_session_t *s, n00b_parse_tree_t *node);
static n00b_cg_val_t codegen_class_decl(n00b_cg_session_t *s, n00b_parse_tree_t *node);
static n00b_cg_val_t codegen_tuple(n00b_cg_session_t *s, n00b_parse_tree_t *node);
static n00b_class_layout_t *compute_class_layout(n00b_cg_session_t *s, n00b_scope_t *scope);
static void    *n00b_builtin_obj_alloc(int64_t size);
static uint64_t n00b_builtin_field_get(void *obj, int64_t offset);
static void     n00b_builtin_field_set(void *obj, int64_t offset, uint64_t value);
static int32_t  layout_field_index(n00b_class_layout_t *layout, const char *name, size_t name_len);

// ============================================================================
// Internal: install default operators on a session
// ============================================================================

static void
install_default_ops(n00b_cg_session_t *s)
{
    s->op_map_cap   = (int32_t)(NUM_DEFAULT_OPS + 16);
    s->op_map_count = (int32_t)NUM_DEFAULT_OPS;
    s->op_map       = n00b_alloc_array(n00b_cg_op_entry_t, (size_t)s->op_map_cap);

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
        s->handlers    = n00b_alloc_array(n00b_cg_handler_fn, (size_t)nt_count);
    }
}

// ============================================================================
// Session lifecycle
// ============================================================================

n00b_cg_session_t *
n00b_cg_session_new(n00b_grammar_t *grammar) _kargs
{
    n00b_cg_import_table_t *imports;
    n00b_cg_type_map_fn     type_map;
    n00b_cg_literal_fn      literal_parser;
    n00b_cg_storage_fn      storage_policy;
    n00b_cg_handler_fn      default_handler;
    void                   *user_data;
    n00b_dict_untyped_t    *embed_registry;
}
{
    n00b_cg_session_t *s = n00b_alloc(n00b_cg_session_t);

    s->grammar         = grammar;
    s->type_map        = kargs->type_map;
    s->literal_parser  = kargs->literal_parser;
    s->storage_policy  = kargs->storage_policy;
    s->default_handler = kargs->default_handler;
    s->user_data       = kargs->user_data;
    s->import_table    = kargs->imports;
    s->diag            = n00b_diag_ctx_new();
    s->global_scope    = n00b_symtab_new();

    // Embed handler registry (caller must provide via .embed_registry karg).
    s->embed_registry = kargs->embed_registry;

    // Module cache: C-string keyed dictionary for use-stmt loading.
    s->module_cache = n00b_alloc(n00b_dict_untyped_t);
    n00b_dict_untyped_init(s->module_cache, .hash = n00b_hash_cstring);

    // Initialize MIR context (single, persistent).
    s->mir_ctx = MIR_init();

    install_handlers(s);
    install_default_ops(s);

    // Register built-in NT handlers.
    if (grammar) {
        n00b_codegen_register(s, "func-def", codegen_func_def);
        n00b_codegen_register(s, "assert-stmt", codegen_assert_stmt);
        n00b_codegen_register(s, "comptime-stmt", codegen_comptime);
        n00b_codegen_register(s, "enum-stmt", codegen_enum_stmt);
        n00b_codegen_register(s, "class-decl", codegen_class_decl);
        n00b_codegen_register(s, "tuple-or-paren", codegen_tuple);
    }

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
n00b_codegen_new(n00b_grammar_t *grammar) _kargs
{
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
                                               .type_map        = kargs->type_map,
                                               .literal_parser  = kargs->literal_parser,
                                               .storage_policy  = kargs->storage_policy,
                                               .default_handler = kargs->default_handler,
                                               .user_data       = kargs->user_data);

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
        int32_t            new_cap  = s->module_cap ? s->module_cap * 2 : 8;
        n00b_cg_module_t **new_mods = n00b_alloc_array(n00b_cg_module_t *, (size_t)new_cap);

        if (s->modules) {
            memcpy(new_mods, s->modules, sizeof(n00b_cg_module_t *) * (size_t)s->module_count);
        }

        s->modules    = new_mods;
        s->module_cap = new_cap;
    }

    s->modules[s->module_count++] = m;
    s->active_module              = m;

    return m;
}

static void *
n00b_cg_module_compile_traced(n00b_cg_module_t *m, const char *entry_func, FILE *code_trace)
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
            MIR_load_external(s->mir_ctx, m->imports[i].name, m->imports[i].addr);
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

        n00b_mir_enable_code_len_trace(s->mir_ctx, code_trace);
        MIR_link(s->mir_ctx, MIR_set_gen_interface, NULL);
        m->state = N00B_CG_MOD_LINKED;
    }

    if (!entry_func) {
        m->state = N00B_CG_MOD_COMPILED;
        return NULL;
    }

    // Find the entry function in this module.
    MIR_item_t func = NULL;

    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, m->mir_module->items); item != NULL;
         item            = DLIST_NEXT(MIR_item_t, item)) {
        if (item->item_type == MIR_func_item && strcmp(item->u.func->name, entry_func) == 0) {
            func = item;
            break;
        }
    }

    if (!func) {
        return NULL;
    }

    void *code = MIR_gen(s->mir_ctx, func);
    m->state   = N00B_CG_MOD_COMPILED;

    return code;
}

void *
n00b_cg_module_compile(n00b_cg_module_t *m, const char *entry_func)
{
    return n00b_cg_module_compile_traced(m, entry_func, NULL);
}

void
n00b_cg_set_active_module(n00b_cg_session_t *s, n00b_cg_module_t *m)
{
    s->active_module = m;
}

void
n00b_cg_module_set_annot(n00b_cg_module_t *m, n00b_annot_result_t *annot)
{
    m->annot = annot;
}

// ============================================================================
// Handler registration
// ============================================================================

bool
n00b_codegen_register(n00b_cg_session_t *s, const char *nt_name, n00b_cg_handler_fn handler)
{
    if (!s->grammar) {
        return false;
    }

    n00b_string_t *name = n00b_string_from_cstr(nt_name);
    bool           found;
    int64_t        id = n00b_dict_get(s->grammar->nt_map, name, &found);

    if (!found) {
        return false;
    }

    return n00b_codegen_register_by_id(s, id, handler);
}

bool
n00b_codegen_register_by_id(n00b_cg_session_t *s, int64_t nt_id, n00b_cg_handler_fn handler)
{
    if (!s->handlers || nt_id < 0 || nt_id >= s->handler_cap) {
        return false;
    }

    s->handlers[nt_id] = handler;
    return true;
}

void
n00b_codegen_map_operator(n00b_cg_session_t    *s,
                          const char           *token_text,
                          n00b_cg_semantic_op_t op)
{
    for (int32_t i = 0; i < s->op_map_count; i++) {
        if (strcmp(s->op_map[i].text, token_text) == 0) {
            s->op_map[i].op = op;
            return;
        }
    }

    if (s->op_map_count >= s->op_map_cap) {
        int32_t             new_cap = s->op_map_cap * 2;
        n00b_cg_op_entry_t *new_map = n00b_alloc_array(n00b_cg_op_entry_t, (size_t)new_cap);
        memcpy(new_map, s->op_map, sizeof(n00b_cg_op_entry_t) * (size_t)s->op_map_count);
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

n00b_tc_type_t *
n00b_codegen_node_tc_type(n00b_cg_session_t *s, n00b_parse_tree_t *node)
{
    n00b_annot_result_t *a = current_annot(s);

    if (!a || !a->node_types) {
        return NULL;
    }

    bool            found;
    uintptr_t       key  = (uintptr_t)node;
    n00b_tc_type_t *type = n00b_dict_get(a->node_types, key, &found);

    if (!found || !type) {
        return NULL;
    }

    return n00b_tc_find(type);
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

n00b_annot_result_t *
n00b_codegen_set_annot(n00b_cg_session_t *s, n00b_annot_result_t *annot)
{
    n00b_annot_result_t *prev = s->annot;
    s->annot = annot;
    return prev;
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

// Emit a codegen-stage diagnostic.
// Uses n00b_diag_push which allocates strings. When calling during
// the codegen tree walk, the GC allocation can trigger conduit crashes
// (known infrastructure issue). For now, we also set the diag error
// count directly as a fallback.
static void
codegen_error(n00b_cg_session_t *s, n00b_parse_tree_t *node,
              const char *code, const char *msg)
{
    n00b_diag_span_t span = n00b_diag_span_from_node(node);

    fprintf(stderr, "error[%s]: %s", code, msg);

    if (span.start_line > 0) {
        fprintf(stderr, " (line %u, col %u)", span.start_line, span.start_col);
    }

    fprintf(stderr, "\n");

    s->has_codegen_errors = true;
}

// ============================================================================
// Default literal parser (strtoll / strtod)
// ============================================================================

static n00b_cg_val_t
default_literal_parser(n00b_cg_session_t *s,
                       n00b_parse_tree_t *node,
                       n00b_string_t     *lit_kind,
                       n00b_cg_type_tag_t type_tag)
{
    const char *text = n00b_pt_token_text(node);
    size_t      len  = n00b_pt_token_text_len(node);

    if (!text || len == 0) {
        return _n00b_cg_const_i64(s, 0);
    }

    char *buf = n00b_alloc_size(1, len + 1);
    memcpy(buf, text, len);
    buf[len] = '\0';

    // Boolean literals.
    if (type_tag == N00B_CG_BOOL || (len == 4 && memcmp(text, "true", 4) == 0)
        || (len == 5 && memcmp(text, "false", 5) == 0)) {
        bool val = (len == 4 && memcmp(text, "true", 4) == 0);
        return _n00b_cg_const_bool(s, val);
    }

    // String literals: type is STRING (or legacy PTR), or text starts
    // with a quote char.
    if (type_tag == N00B_CG_STRING || type_tag == N00B_CG_PTR
        || (len >= 2 && (text[0] == '"' || text[0] == '\''))) {
        // Strip quotes if present.
        const char *str_data = buf;
        size_t      str_len  = len;

        if (str_len >= 2 && (str_data[0] == '"' || str_data[0] == '\'')) {
            str_data++;
            str_len -= 2;
        }

        // Create an n00b_string_t so the value is a managed object.
        n00b_string_t *str_obj = n00b_string_from_raw(str_data, (int64_t)str_len);

        return (n00b_cg_val_t){
            .kind     = N00B_CG_VAL_IMM,
            .type_tag = N00B_CG_STRING,
            .aux      = (uint64_t)(uintptr_t)str_obj,
        };
    }

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
static n00b_cg_val_t
codegen_literal(n00b_cg_session_t *s, n00b_parse_tree_t *node, n00b_annotation_t *annot);
static n00b_cg_val_t
codegen_call_auto(n00b_cg_session_t *s, n00b_parse_tree_t *node, n00b_annotation_t *annot);
static n00b_cg_val_t codegen_branch(n00b_cg_session_t *s, n00b_cf_label_t *cf);
static n00b_cg_val_t codegen_loop(n00b_cg_session_t *s, n00b_cf_label_t *cf);
static n00b_cg_val_t codegen_jump(n00b_cg_session_t *s, n00b_cf_label_t *cf);
static n00b_cg_val_t codegen_assign(n00b_cg_session_t *s, n00b_cf_label_t *cf);
static n00b_cg_val_t codegen_varref(n00b_cg_session_t *s, n00b_cf_label_t *cf);
static n00b_cg_val_t codegen_switch(n00b_cg_session_t *s, n00b_cf_label_t *cf);
static n00b_cg_val_t codegen_unwrap_result(n00b_cg_session_t *s, n00b_cf_label_t *cf);
static n00b_cg_val_t codegen_call_cf(n00b_cg_session_t *s, n00b_cf_label_t *cf);
static n00b_cg_val_t codegen_func_def(n00b_cg_session_t *s, n00b_parse_tree_t *node);
static n00b_cg_val_t codegen_assert_stmt(n00b_cg_session_t *s, n00b_parse_tree_t *node);
static n00b_cg_val_t codegen_children_default(n00b_cg_session_t *s, n00b_parse_tree_t *node);

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
    }
    else {
        n00b_cg_emit_bt(s, left_val, short_label);
    }

    n00b_cg_val_t right_val = codegen_walk(s, right);
    n00b_cg_store(s, result, right_val);
    n00b_cg_emit_jmp(s, end_label);

    n00b_cg_label_here(s, short_label);

    if (op == N00B_CG_OP_LOGICAL_AND) {
        n00b_cg_store(s, result, _n00b_cg_const_i64(s, 0));
    }
    else {
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
            return codegen_short_circuit(s,
                                         (n00b_cg_semantic_op_t)sem_op,
                                         operands[0],
                                         operands[1]);
        }

        n00b_cg_val_t a = codegen_walk(s, operands[0]);
        n00b_cg_val_t b = codegen_walk(s, operands[1]);

        return n00b_cg_emit_binop(s, (n00b_cg_semantic_op_t)sem_op, a, b);
    }

    if (n_operands == 1) {
        // Unary '-' is negation, not subtraction.
        if (sem_op == N00B_CG_OP_SUB) {
            sem_op = N00B_CG_OP_NEG;
        }

        n00b_cg_val_t a = codegen_walk(s, operands[0]);
        return n00b_cg_emit_unop(s, (n00b_cg_semantic_op_t)sem_op, a);
    }

    return codegen_children_default(s, node);
}

// ============================================================================
// Literal codegen
// ============================================================================

static n00b_cg_val_t
codegen_literal(n00b_cg_session_t *s, n00b_parse_tree_t *node, n00b_annotation_t *annot)
{
    n00b_cg_type_tag_t type = n00b_codegen_node_type(s, node);

    n00b_parse_tree_t *tok_node = n00b_pt_first_token(node);

    if (!tok_node) {
        return _n00b_cg_const_i64(s, 0);
    }

    n00b_string_t *lit_kind = annot->op_kind;

    if (s->literal_parser) {
        return s->literal_parser(s, tok_node, lit_kind, type);
    }

    return default_literal_parser(s, tok_node, lit_kind, type);
}

// ============================================================================
// Embed literal codegen
// ============================================================================

static n00b_cg_val_t
codegen_embed(n00b_cg_session_t *s, n00b_parse_tree_t *node)
{
    // Find the EMBED token (first token child).
    n00b_parse_tree_t *tok_node = n00b_pt_first_token(node);
    n00b_token_info_t *tok      = tok_node ? n00b_parse_node_token(tok_node) : nullptr;

    if (!tok || !n00b_option_is_set(tok->value)) {
        return N00B_CG_VOID_VAL;
    }

    n00b_string_t *content = n00b_option_get(tok->value);

    // Extract modifier.  The tokenizer stores the 'ffi suffix in
    // tok->modifier.  Fall back to a <type-spec> grammar child if the
    // tokenizer didn't capture it (shouldn't happen in practice).
    n00b_string_t *modifier = nullptr;

    if (n00b_option_is_set(tok->modifier)) {
        modifier = n00b_option_get(tok->modifier);
    }
    else {
        n00b_parse_tree_t *ts_node = n00b_pt_find_child_by_nt(node, "type-spec");
        if (ts_node) {
            n00b_parse_tree_t *id_tok = n00b_pt_first_token(ts_node);

            if (id_tok) {
                const char *text = n00b_pt_token_text(id_tok);
                size_t      tlen = n00b_pt_token_text_len(id_tok);

                if (text && tlen > 0) {
                    modifier = n00b_string_from_raw(text, (int64_t)tlen);
                }
            }
        }
    }

    // TODO: If no modifier, try to get the type from inference.
    // For now, without a modifier we can't dispatch.
    if (!modifier) {
        codegen_error(s, node, "CG001",
                      "embed literal: no type modifier specified");
        return N00B_CG_VOID_VAL;
    }

    if (!s->embed_registry) {
        codegen_error(s, node, "CG002",
                      "embed literal: no embed registry configured");
        return N00B_CG_VOID_VAL;
    }

    n00b_embed_result_t r = n00b_embed_dispatch(s->embed_registry, s, content, modifier);

    // Cast the opaque 16-byte result back to n00b_cg_val_t.
    n00b_cg_val_t val;
    _Static_assert(sizeof(val) == sizeof(r),
                   "n00b_cg_val_t and n00b_embed_result_t must be same size");
    memcpy(&val, &r, sizeof(val));
    return val;
}

// ============================================================================
// Auto-inferred call codegen
// ============================================================================

static n00b_cg_val_t
codegen_call_auto(n00b_cg_session_t *s, n00b_parse_tree_t *node, n00b_annotation_t *annot)
{
    n00b_parse_tree_t *func_node = NULL;
    n00b_parse_tree_t *args_node = NULL;

    if (annot->name_ref.kind == N00B_ROLE_BY_INDEX) {
        int32_t idx = annot->name_ref.index;

        if (idx >= 0) {
            func_node = n00b_pt_get_child(node, (size_t)idx);
        }
    }

    if (annot->value_ref.kind == N00B_ROLE_BY_INDEX && annot->value_ref.index >= 0) {
        args_node = n00b_pt_get_child(node, (size_t)annot->value_ref.index);
    }
    else if (annot->type_ref.kind == N00B_ROLE_BY_INDEX && annot->type_ref.index >= 0) {
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
            }
            else if (n00b_pt_is_group(arg)) {
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

    return n00b_cg_emit_call(s, func_name, args, n_args, .ret = ret_type);
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

// Check if a for-loop's condition is a range node (for x in start to end).
static bool
is_range_loop(n00b_cf_label_t *cf)
{
    if (!cf->cond) {
        return false;
    }

    return n00b_pt_is_nt(cf->cond, "range");
}

// Extract the loop variable name from a for-stmt node.
// for-stmt children: 'for', for-var-list, 'in'/'from', range/target, body
static const char *
extract_loop_var_name(n00b_cg_session_t *s, n00b_cf_label_t *cf)
{
    n00b_parse_tree_t *self = cf->self;
    size_t nc = n00b_pt_num_children(self);

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(self, i);

        if (child && n00b_pt_is_nt(child, "for-var-list")) {
            n00b_parse_tree_t *tok = n00b_pt_first_token(child);

            if (tok) {
                const char *text = n00b_pt_token_text(tok);
                size_t      len  = n00b_pt_token_text_len(tok);

                if (text && len > 0) {
                    char *buf = n00b_alloc_size(1, len + 1);
                    memcpy(buf, text, len);
                    buf[len] = '\0';
                    return buf;
                }
            }
        }
    }

    return NULL;
}

// Extract start and end expressions from a range node.
// range -> range-parts -> expression, range-sep, expression
static bool
extract_range_bounds(n00b_cg_session_t  *s,
                     n00b_parse_tree_t  *range_node,
                     n00b_parse_tree_t **start_out,
                     n00b_parse_tree_t **end_out)
{
    // range -> range-parts (single child or direct)
    n00b_parse_tree_t *parts = range_node;

    // Descend through range to range-parts if needed.
    size_t nc = n00b_pt_num_children(parts);

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(parts, i);

        if (child && n00b_pt_is_nt(child, "range-parts")) {
            parts = child;
            break;
        }
    }

    // range-parts has children: expression, range-sep, expression
    // Find the first two non-token, non-group NTs that are expressions.
    n00b_parse_tree_t *exprs[2] = {NULL, NULL};
    int n_exprs = 0;

    nc = n00b_pt_num_children(parts);

    for (size_t i = 0; i < nc && n_exprs < 2; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(parts, i);

        if (!child || n00b_pt_is_token(child)) {
            continue;
        }

        if (n00b_pt_is_group(child)) {
            continue;
        }

        // Skip range-sep nodes.
        if (n00b_pt_is_nt(child, "range-sep")) {
            continue;
        }

        exprs[n_exprs++] = child;
    }

    if (n_exprs < 2) {
        return false;
    }

    *start_out = exprs[0];
    *end_out   = exprs[1];
    return true;
}

// Push a loop entry onto the loop stack.
static void
push_loop(n00b_cg_session_t *s, n00b_cg_val_t break_label,
          n00b_cg_val_t continue_label)
{
    n00b_cg_module_t *m = s->active_module;

    if (m->loop_depth >= m->loop_cap) {
        int32_t               new_cap = m->loop_cap ? m->loop_cap * 2 : 8;
        n00b_cg_loop_entry_t *new_stack
            = n00b_alloc_array(n00b_cg_loop_entry_t, (size_t)new_cap);
        if (m->loop_stack) {
            memcpy(new_stack,
                   m->loop_stack,
                   sizeof(n00b_cg_loop_entry_t) * (size_t)m->loop_depth);
        }

        m->loop_stack = new_stack;
        m->loop_cap   = new_cap;
    }

    m->loop_stack[m->loop_depth++] = (n00b_cg_loop_entry_t){
        .break_label    = break_label,
        .continue_label = continue_label,
    };
}

static n00b_cg_val_t
codegen_loop(n00b_cg_session_t *s, n00b_cf_label_t *cf)
{
    n00b_cg_module_t *m = s->active_module;

    // Check if this is a for-in-range loop.
    if (is_range_loop(cf)) {
        const char *var_name = extract_loop_var_name(s, cf);

        n00b_parse_tree_t *start_node = NULL;
        n00b_parse_tree_t *end_node   = NULL;

        if (!var_name || !extract_range_bounds(s, cf->cond,
                                                &start_node, &end_node)) {
            return N00B_CG_VOID_VAL;
        }

        // Evaluate start and end bounds.
        n00b_cg_val_t start_val = codegen_walk(s, start_node);
        n00b_cg_val_t end_val   = codegen_walk(s, end_node);

        // Create the loop variable, initialize to start.
        n00b_cg_val_t loop_var = n00b_cg_local(s, var_name,
                                                .type = N00B_CG_I64);
        n00b_cg_store(s, loop_var, start_val);

        // Store end in a temp so it's not re-evaluated each iteration.
        n00b_cg_val_t end_tmp = n00b_cg_temp(s, N00B_CG_I64);
        n00b_cg_store(s, end_tmp, end_val);

        n00b_cg_val_t test_label     = n00b_cg_label_new(s);
        n00b_cg_val_t break_label    = n00b_cg_label_new(s);
        n00b_cg_val_t continue_label = n00b_cg_label_new(s);

        push_loop(s, break_label, continue_label);

        // Test: loop_var < end
        n00b_cg_label_here(s, test_label);
        n00b_cg_val_t cond = n00b_cg_emit_binop(s, N00B_CG_OP_LT,
                                                  loop_var, end_tmp);
        n00b_cg_emit_bf(s, cond, break_label);

        // Body.
        if (cf->then_body) {
            codegen_walk(s, cf->then_body);
        }

        // Continue: increment and jump to test.
        n00b_cg_label_here(s, continue_label);
        n00b_cg_val_t one = _n00b_cg_const_i64(s, 1);
        n00b_cg_val_t inc = n00b_cg_emit_binop(s, N00B_CG_OP_ADD,
                                                loop_var, one);
        n00b_cg_store(s, loop_var, inc);
        n00b_cg_emit_jmp(s, test_label);

        n00b_cg_label_here(s, break_label);

        m->loop_depth--;
        return N00B_CG_VOID_VAL;
    }

    // While-style loop: condition + body.
    n00b_cg_val_t continue_label = n00b_cg_label_new(s);
    n00b_cg_val_t break_label    = n00b_cg_label_new(s);

    push_loop(s, break_label, continue_label);

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
    n00b_cg_module_t *m  = s->active_module;
    const char       *jk = cf->jump_kind ? cf->jump_kind->data : NULL;

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

// Compound assignment operator table: token text -> base operator text.
static const struct {
    const char *compound;
    const char *base;
} compound_assign_ops[] = {
    {"+=", "+"},
    {"-=", "-"},
    {"*=", "*"},
    {"/=", "/"},
    {"%=", "%"},
    {"&=", "&"},
    {"|=", "|"},
    {"^=", "^"},
    {"<<=", "<<"},
    {">>=", ">>"},
};

#define NUM_COMPOUND_OPS (sizeof(compound_assign_ops) / sizeof(compound_assign_ops[0]))

// Find compound assignment operator token in cf->self children.
// Returns the base operator string (e.g. "+" for "+="), or NULL for simple "=".
static const char *
find_compound_op(n00b_parse_tree_t *self_node)
{
    size_t nc = n00b_pt_num_children(self_node);

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(self_node, i);

        // The operator is in child $1 (<binop-assign-op>), which may
        // be a non-terminal containing a single token, or the token
        // itself might be directly found via first_token.
        n00b_parse_tree_t *tok = n00b_pt_first_token(child);

        if (!tok) {
            continue;
        }

        const char *text = n00b_pt_token_text(tok);
        size_t      tlen = n00b_pt_token_text_len(tok);

        if (!text || tlen == 0) {
            continue;
        }

        for (size_t j = 0; j < NUM_COMPOUND_OPS; j++) {
            size_t clen = strlen(compound_assign_ops[j].compound);

            if (tlen == clen && memcmp(text, compound_assign_ops[j].compound, clen) == 0) {
                return compound_assign_ops[j].base;
            }
        }
    }

    return NULL;
}

// Track semantic type tags for local variables.
static n00b_cg_val_t
codegen_assign(n00b_cg_session_t *s, n00b_cf_label_t *cf)
{
    n00b_cg_val_t value = N00B_CG_VOID_VAL;

    if (cf->then_body) {
        value = codegen_walk(s, cf->then_body);
    }

    // Check for field assignment: self.x = val or obj.field = val.
    // The cf->cond is the <expression> wrapping the LHS. We need to
    // find the innermost postfix-expr that has a '.' token.
    if (cf->cond) {
        // Walk down expression wrappers to find the postfix-expr with a dot.
        n00b_parse_tree_t *dot_node = NULL;
        {
            n00b_parse_tree_t *walk_stack[64];
            int walk_sp = 0;
            walk_stack[walk_sp++] = cf->cond;

            while (walk_sp > 0 && !dot_node) {
                n00b_parse_tree_t *cur = walk_stack[--walk_sp];

                if (!cur || n00b_pt_is_token(cur)) {
                    continue;
                }

                // Check if this node has a '.' token child.
                size_t cnc = n00b_pt_num_children(cur);

                for (size_t ci = 0; ci < cnc; ci++) {
                    n00b_parse_tree_t *cc = n00b_pt_get_child(cur, ci);

                    if (n00b_pt_is_token(cc)) {
                        const char *ct = n00b_pt_token_text(cc);
                        size_t ctl = n00b_pt_token_text_len(cc);

                        if (ctl == 1 && ct[0] == '.') {
                            dot_node = cur;
                            break;
                        }
                    }
                }

                if (!dot_node) {
                    // Push children for deeper search.
                    for (size_t ci = 0; ci < cnc && walk_sp < 63; ci++) {
                        n00b_parse_tree_t *cc = n00b_pt_get_child(cur, ci);

                        if (cc && !n00b_pt_is_token(cc)) {
                            walk_stack[walk_sp++] = cc;
                        }
                    }
                }
            }
        }

        bool has_dot = false;
        n00b_parse_tree_t *fa_lhs = NULL;
        const char *fa_field = NULL;
        size_t fa_field_len = 0;

        if (dot_node) {
            size_t anc = n00b_pt_num_children(dot_node);

            for (size_t i = 0; i < anc; i++) {
                n00b_parse_tree_t *child = n00b_pt_get_child(dot_node, i);

                if (n00b_pt_is_token(child)) {
                    const char *t = n00b_pt_token_text(child);
                    size_t tl = n00b_pt_token_text_len(child);

                    if (tl == 1 && t[0] == '.') {
                        has_dot = true;
                    }
                    else if (has_dot && tl > 0) {
                        fa_field = t;
                        fa_field_len = tl;
                    }
                }
                else if (!has_dot) {
                    fa_lhs = child;
                }
            }
        }

        if (has_dot && fa_lhs && fa_field && fa_field_len > 0) {
            // Field assignment: evaluate receiver, find layout, emit field_set.
            n00b_cg_val_t obj = codegen_walk(s, fa_lhs);

            if (obj.kind != N00B_CG_VAL_VOID) {
                n00b_annot_result_t *ar = current_annot(s);
                n00b_sym_entry_t *type_sym = NULL;

                if (ar && ar->symtab) {
                    // Try type-based lookup.
                    n00b_tc_type_t *lhs_type = n00b_codegen_node_tc_type(s, fa_lhs);

                    if (lhs_type
                        && n00b_variant_is_type(lhs_type->kind, n00b_tc_prim_t)) {
                        n00b_tc_prim_t prim = n00b_variant_get(
                            lhs_type->kind, n00b_tc_prim_t);

                        if (prim.name && prim.name->u8_bytes > 0) {
                            type_sym = n00b_symtab_lookup_any(
                                ar->symtab, n00b_string_empty(), prim.name);
                        }
                    }

                    // Fallback for self inside method.
                    if (!type_sym) {
                        n00b_parse_tree_t *vt = n00b_pt_first_token(fa_lhs);

                        if (vt) {
                            const char *vn = n00b_pt_token_text(vt);
                            size_t vl = n00b_pt_token_text_len(vt);

                            if (vn && vl == 4 && memcmp(vn, "self", 4) == 0) {
                                n00b_cg_module_t *m = s->active_module;

                                if (m && m->cur_func) {
                                    const char *fname = m->cur_func->u.func->name;
                                    const char *dollar = strchr(fname, '$');

                                    if (dollar) {
                                        size_t clen = (size_t)(dollar - fname);
                                        n00b_string_t *cname = n00b_string_from_raw(
                                            fname, (int64_t)clen);
                                        type_sym = n00b_symtab_lookup_any(
                                            ar->symtab, n00b_string_empty(),
                                            cname);
                                    }
                                }
                            }
                        }
                    }
                }

                if (type_sym && type_sym->exposed_scope) {
                    if (!type_sym->class_layout) {
                        type_sym->class_layout = compute_class_layout(
                            s, type_sym->exposed_scope);
                    }

                    n00b_class_layout_t *layout = type_sym->class_layout;

                    if (layout) {
                        int32_t fidx = layout_field_index(
                            layout, fa_field, fa_field_len);

                        if (fidx >= 0) {
                            n00b_cg_type_tag_t set_pt[] = {
                                N00B_CG_I64, N00B_CG_I64, N00B_CG_I64};
                            n00b_cg_import_func(
                                s, "n00b_builtin_field_set",
                                (void *)n00b_builtin_field_set,
                                .ret = N00B_CG_VOID,
                                .param_types = set_pt,
                                .n_params = 3);
                            n00b_cg_val_t offset_arg = _n00b_cg_const_i64(
                                s, (int64_t)layout->field_offsets[fidx]);
                            n00b_cg_val_t set_args[] = {
                                obj, offset_arg, value};
                            n00b_cg_emit_call(
                                s, "n00b_builtin_field_set",
                                set_args, 3, .ret = N00B_CG_VOID);
                            return value;
                        }
                    }
                }
            }
        }
    }

    if (cf->cond) {
        n00b_parse_tree_t *name_tok = n00b_pt_first_token(cf->cond);

        if (name_tok) {
            const char *var_name = n00b_pt_token_text(name_tok);

            if (var_name) {
                // Check for compound assignment (+=, -=, etc.).
                const char *base_op = find_compound_op(cf->self);

                if (base_op) {
                    // Compound: load current value, apply binop, store back.
                    n00b_cg_module_t *m    = s->active_module;
                    MIR_func_t        func = m->cur_func->u.func;
                    size_t            nlen = n00b_pt_token_text_len(name_tok);
                    char              nbuf[128];

                    if (nlen >= sizeof(nbuf)) {
                        nlen = sizeof(nbuf) - 1;
                    }

                    memcpy(nbuf, var_name, nlen);
                    nbuf[nlen] = '\0';

                    MIR_reg_t     reg = MIR_reg(s->mir_ctx, nbuf, func);
                    n00b_cg_val_t lhs = (n00b_cg_val_t){
                        .id       = (uint32_t)reg,
                        .kind     = N00B_CG_VAL_REG,
                        .type_tag = value.type_tag,
                    };

                    int32_t sem_op = n00b_cg_lookup_op(s, base_op);

                    if (sem_op >= 0) {
                        n00b_cg_val_t result
                            = n00b_cg_emit_binop(s, (n00b_cg_semantic_op_t)sem_op, lhs, value);
                        n00b_cg_store(s, lhs, result);
                        return lhs;
                    }
                }

                // Simple assignment: try to look up existing register first,
                // only create a new local if it doesn't exist yet.
                n00b_cg_module_t *m2    = s->active_module;
                MIR_func_t        fn2   = m2->cur_func->u.func;
                size_t            nlen2 = n00b_pt_token_text_len(name_tok);
                char              nbuf2[128];

                if (nlen2 >= sizeof(nbuf2)) {
                    nlen2 = sizeof(nbuf2) - 1;
                }

                memcpy(nbuf2, var_name, nlen2);
                nbuf2[nlen2] = '\0';

                n00b_cg_val_t dst;
                MIR_reg_t     existing  = 0;
                bool          found_reg = false;

                // Check if register already exists (re-assignment).
                for (uint32_t ri = 0; ri < VARR_LENGTH(MIR_var_t, fn2->vars); ri++) {
                    MIR_var_t v = VARR_GET(MIR_var_t, fn2->vars, ri);

                    if (v.name && strcmp(v.name, nbuf2) == 0) {
                        existing  = MIR_reg(s->mir_ctx, nbuf2, fn2);
                        found_reg = true;
                        break;
                    }
                }

                if (found_reg) {
                    dst = (n00b_cg_val_t){
                        .id       = (uint32_t)existing,
                        .kind     = N00B_CG_VAL_REG,
                        .type_tag = value.type_tag,
                    };
                }
                else {
                    dst = n00b_cg_local(s, nbuf2, .type = value.type_tag);
                }

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

                // Try node_types first (has the properly unified type
                // from the annotation walk). If not found, look up
                // the sym entry and try to resolve through the type
                // system. The symtab entry's type_var may not be
                // unified for variables declared without explicit types.
                n00b_cg_type_tag_t tag = n00b_codegen_node_type(s, cf->self);


                return (n00b_cg_val_t){
                    .id       = (uint32_t)reg,
                    .kind     = N00B_CG_VAL_REG,
                    .type_tag = tag,
                };
            }
        }
    }

    return N00B_CG_VOID_VAL;
}

// ============================================================================
// Unwrap result (postfix !)
// ============================================================================

static n00b_cg_val_t
codegen_unwrap_result(n00b_cg_session_t *s, n00b_cf_label_t *cf)
{
    // Grammar: <postfix-expr> ::= <postfix-expr> %"!"
    // Evaluate the operand, then call the appropriate unwrap helper.
    // For now: aborts on err/none. TODO: propagation in result/option-
    // returning functions.
    n00b_cg_val_t operand = N00B_CG_VOID_VAL;
    size_t nc = n00b_pt_num_children(cf->self);

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(cf->self, i);

        if (!n00b_pt_is_token(child)) {
            operand = codegen_walk(s, child);
            break;
        }
    }

    if (operand.kind == N00B_CG_VAL_VOID) {
        return N00B_CG_VOID_VAL;
    }

    // Dispatch based on the operand's type tag.
    const char *name;
    void       *addr;

    if (operand.type_tag == N00B_CG_OPTION) {
        name = "n00b_builtin_option_unwrap";
        addr = (void *)n00b_builtin_option_unwrap;
    }
    else if (operand.type_tag == N00B_CG_RESULT) {
        name = "n00b_builtin_result_unwrap";
        addr = (void *)n00b_builtin_result_unwrap;
    }
    else {
        // Not an option or result — just pass through.
        return operand;
    }

    n00b_cg_type_tag_t pt[] = {N00B_CG_I64};
    n00b_cg_import_func(s, name, addr,
                         .ret = N00B_CG_I64,
                         .param_types = pt, .n_params = 1);
    return n00b_cg_emit_call(s, name, &operand, 1, .ret = N00B_CG_I64);
}

// ============================================================================
// CF_CALL: function call via control flow label
// ============================================================================

// Check if a callee node is a method call (has a '.' token child).
// If so, return the receiver subtree and the method name.
static bool
is_method_call(n00b_parse_tree_t  *callee,
               n00b_parse_tree_t **receiver_out,
               const char        **method_out)
{
    if (!callee || n00b_pt_is_token(callee)) {
        return false;
    }

    // Look for a '.' token among children. If found, the child before
    // it is the receiver and the token after it is the method name.
    size_t nc = n00b_pt_num_children(callee);
    bool   found_dot = false;

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(callee, i);

        if (n00b_pt_is_token(child)) {
            const char *text = n00b_pt_token_text(child);
            size_t      len  = n00b_pt_token_text_len(child);

            if (text && len == 1 && text[0] == '.') {
                found_dot = true;
                continue;
            }

            // Token after '.' is the method name.
            if (found_dot && text && len > 0) {
                char *buf = n00b_alloc_size(1, len + 1);
                memcpy(buf, text, len);
                buf[len] = '\0';
                *method_out = buf;
            }
        }
        else if (!found_dot) {
            *receiver_out = child;
        }
    }

    return found_dot && *receiver_out && *method_out;
}

static n00b_cg_val_t
codegen_call_cf(n00b_cg_session_t *s, n00b_cf_label_t *cf)
{
    // CF_CALL has: cond = callee node, then_body = args node.
    // Check for method call (expr.name) vs plain function call (name).
    const char        *func_name = NULL;
    n00b_parse_tree_t *receiver  = NULL;
    const char        *method    = NULL;
    n00b_cg_val_t      recv_val  = N00B_CG_VOID_VAL;

    if (cf->cond && is_method_call(cf->cond, &receiver, &method)) {
        recv_val  = codegen_walk(s, receiver);
        func_name = method;
    }
    else if (cf->cond) {
        n00b_parse_tree_t *name_tok = n00b_pt_first_token(cf->cond);

        if (name_tok) {
            const char *raw = n00b_pt_token_text(name_tok);
            size_t      len = n00b_pt_token_text_len(name_tok);

            if (raw && len > 0) {
                char *buf = n00b_alloc_size(1, len + 1);
                memcpy(buf, raw, len);
                buf[len]  = '\0';
                func_name = buf;
            }
        }
    }

    if (!func_name) {
        return codegen_children_default(s, cf->self);
    }

    // Collect arguments from the args node.
    // For method calls, the receiver is the implicit first argument.
    n00b_cg_val_t args[32];
    int32_t       n_args = 0;

    if (recv_val.kind != N00B_CG_VAL_VOID) {
        args[n_args++] = recv_val;
    }

    if (cf->then_body) {
        n00b_parse_tree_t *arg_stack[64];
        int                arg_sp = 0;

        // Push call-args children in reverse for L-to-R order.
        size_t nc = n00b_pt_num_children(cf->then_body);

        for (size_t i = nc; i > 0; i--) {
            if (arg_sp < 64) {
                arg_stack[arg_sp++] = n00b_pt_get_child(cf->then_body, i - 1);
            }
        }

        while (arg_sp > 0 && n_args < 32) {
            n00b_parse_tree_t *cur = arg_stack[--arg_sp];

            if (!cur || n00b_pt_is_token(cur)) {
                continue; // Skip commas, parens, etc.
            }

            n00b_nt_node_t *cpn = &n00b_tree_node_value(cur);

            if (cpn->group_top) {
                // Group node: recurse into its children.
                size_t gnc = n00b_pt_num_children(cur);

                for (size_t i = gnc; i > 0; i--) {
                    if (arg_sp < 64) {
                        arg_stack[arg_sp++] = n00b_pt_get_child(cur, i - 1);
                    }
                }

                continue;
            }

            // Non-token, non-group NT: this is an argument expression.
            n00b_cg_val_t val = codegen_walk(s, cur);

            if (val.kind != N00B_CG_VAL_VOID) {
                args[n_args++] = val;
            }
        }
    }

    // Check for built-in functions before falling through to generic
    // MIR call emission.
    n00b_cg_val_t builtin_result;

    if (n00b_codegen_builtin_call(s, func_name, args, n_args,
                                   &builtin_result)) {
        return builtin_result;
    }

    // For method calls, try vtable dispatch (builtin methods).
    if (recv_val.kind != N00B_CG_VAL_VOID) {
        if (n00b_codegen_method_dispatch(s, func_name, args, n_args,
                                          &builtin_result)) {
            return builtin_result;
        }
    }

    // For method calls, try class method dispatch (static binding).
    if (recv_val.kind != N00B_CG_VAL_VOID && receiver) {
        n00b_annot_result_t *ar = current_annot(s);

        if (ar && ar->symtab) {
            n00b_tc_type_t *recv_type = n00b_codegen_node_tc_type(s, receiver);
            n00b_sym_entry_t *type_sym = NULL;

            if (recv_type
                && n00b_variant_is_type(recv_type->kind, n00b_tc_prim_t)) {
                n00b_tc_prim_t prim = n00b_variant_get(
                    recv_type->kind, n00b_tc_prim_t);

                if (prim.name && prim.name->u8_bytes > 0) {
                    type_sym = n00b_symtab_lookup_any(
                        ar->symtab, n00b_string_empty(), prim.name);
                }
            }

            if (type_sym && type_sym->exposed_scope
                && type_sym->exposed_scope->scope_tag
                && (type_sym->exposed_scope->scope_tag->u8_bytes == 5
                     && memcmp(type_sym->exposed_scope->scope_tag->data,
                               "class", 5) == 0)) {
                if (!type_sym->class_layout) {
                    type_sym->class_layout = compute_class_layout(
                        s, type_sym->exposed_scope);
                }

                n00b_class_layout_t *layout = type_sym->class_layout;

                if (layout) {
                    // Find the method in the layout.
                    for (uint32_t mi = 0; mi < layout->n_methods; mi++) {
                        if (strcmp(layout->method_names[mi], func_name) == 0) {
                            // Emit direct call to the mangled MIR function.
                            // args[0] is already the receiver (self).
                            n00b_cg_type_tag_t ret = n00b_codegen_node_type(
                                s, cf->self);

                            if (ret == N00B_CG_PTR) {
                                ret = N00B_CG_I64;
                            }

                            return n00b_cg_emit_call(
                                s, layout->method_mir_names[mi],
                                args, n_args, .ret = ret);
                        }
                    }
                }
            }
        }
    }

    // Check for class constructor call.
    // Only for classes (scope_tag == "class"), not tuples.
    {
        n00b_annot_result_t *ar = current_annot(s);

        if (ar && ar->symtab) {
            n00b_string_t *fn_str = n00b_string_from_cstr(func_name);
            n00b_sym_entry_t *sym = n00b_symtab_lookup_any(
                ar->symtab, n00b_string_empty(), fn_str);

            // Guard: only classes, not tuples.
            bool is_class = sym && sym->exposed_scope
                && sym->exposed_scope->scope_tag
                && (sym->exposed_scope->scope_tag->u8_bytes == 5
                     && memcmp(sym->exposed_scope->scope_tag->data,
                               "class", 5) == 0);

            if (is_class) {
                if (!sym->class_layout) {
                    sym->class_layout = compute_class_layout(
                        s, sym->exposed_scope);
                }

                n00b_class_layout_t *layout = sym->class_layout;

                // Allocate instance.
                n00b_cg_type_tag_t alloc_pt[] = {N00B_CG_I64};
                n00b_cg_import_func(s, "n00b_builtin_obj_alloc",
                                     (void *)n00b_builtin_obj_alloc,
                                     .ret = N00B_CG_I64,
                                     .param_types = alloc_pt, .n_params = 1);
                n00b_cg_val_t size_arg = _n00b_cg_const_i64(
                    s, (int64_t)layout->instance_size);
                n00b_cg_val_t obj = n00b_cg_emit_call(
                    s, "n00b_builtin_obj_alloc", &size_arg, 1,
                    .ret = N00B_CG_I64);
                obj.type_tag = N00B_CG_PTR;

                if (layout->has_init) {
                    // Call init: ClassName$init(obj, arg0, arg1, ...)
                    // Find the init method's MIR name.
                    const char *init_mir = NULL;

                    for (uint32_t mi = 0; mi < layout->n_methods; mi++) {
                        if (strcmp(layout->method_names[mi], "init") == 0) {
                            init_mir = layout->method_mir_names[mi];
                            break;
                        }
                    }

                    if (init_mir) {
                        // Build args: [obj, original_args...]
                        n00b_cg_val_t init_args[n_args + 1];
                        init_args[0] = obj;

                        for (int32_t i = 0; i < n_args; i++) {
                            init_args[i + 1] = args[i];
                        }

                                        n00b_cg_emit_call(s, init_mir, init_args, n_args + 1,
                                           .ret = N00B_CG_I64);
                    }
                }
                else {
                    // No init — auto-assign positional args to fields.
                    n00b_cg_type_tag_t set_pt[] = {
                        N00B_CG_I64, N00B_CG_I64, N00B_CG_I64};
                    n00b_cg_import_func(s, "n00b_builtin_field_set",
                                         (void *)n00b_builtin_field_set,
                                         .ret = N00B_CG_VOID,
                                         .param_types = set_pt, .n_params = 3);

                    for (int32_t i = 0; i < n_args
                             && (uint32_t)i < layout->n_fields; i++) {
                        n00b_cg_val_t offset_arg = _n00b_cg_const_i64(
                            s, (int64_t)layout->field_offsets[i]);
                        n00b_cg_val_t set_args[] = {obj, offset_arg, args[i]};
                        n00b_cg_emit_call(s, "n00b_builtin_field_set",
                                           set_args, 3, .ret = N00B_CG_VOID);
                    }
                }

                return obj;
            }
        }
    }

    n00b_cg_type_tag_t ret_type = n00b_codegen_node_type(s, cf->self);

    // If the node type is unresolved (mapped to PTR for Var types),
    // default to I64 since MIR call protos need a concrete type and
    // I64 is the most common return type for user functions.
    if (ret_type == N00B_CG_PTR) {
        ret_type = N00B_CG_I64;
    }

    return n00b_cg_emit_call(s, func_name, args, n_args, .ret = ret_type);
}

// ============================================================================
// Switch codegen
// ============================================================================

// Forward: walk a branch-list node recursively to emit case arms.
static void codegen_switch_branch_list(n00b_cg_session_t *s,
                                       n00b_parse_tree_t *branch_list,
                                       n00b_cg_val_t      switch_val,
                                       n00b_cg_val_t      end_label);

static void
codegen_switch_case_block(n00b_cg_session_t *s,
                          n00b_parse_tree_t *case_block,
                          n00b_cg_val_t      switch_val,
                          n00b_cg_val_t      end_label,
                          n00b_cg_val_t      next_label)
{
    // <switch-case-block> ::= <case-expr-list> <case-body>
    //                       | <case-expr-list> <body>
    // First non-token child is the case-expr-list, second is body.
    n00b_parse_tree_t *expr_list = NULL;
    n00b_parse_tree_t *body      = NULL;
    size_t             nc        = n00b_pt_num_children(case_block);

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(case_block, i);

        if (n00b_pt_is_token(child)) {
            continue;
        }

        if (!expr_list) {
            expr_list = child;
        }
        else {
            body = child;
            break;
        }
    }

    // Evaluate case expressions and emit comparisons.
    // Each case-expr-item may be a simple expression or a range.
    n00b_cg_val_t match_label = n00b_cg_label_new(s);

    if (expr_list) {
        // Walk case-expr-list children: each non-token child is a
        // case-expr-item that might be a simple value or range.
        size_t enc = n00b_pt_num_children(expr_list);

        for (size_t i = 0; i < enc; i++) {
            n00b_parse_tree_t *item = n00b_pt_get_child(expr_list, i);

            if (n00b_pt_is_token(item)) {
                continue; // Skip commas.
            }

            // Check if this is a range (has 3+ children with a
            // separator token like "to" between two expressions).
            size_t             inc           = n00b_pt_num_children(item);
            n00b_parse_tree_t *exprs[2]      = {NULL, NULL};
            int                n_exprs       = 0;
            bool               has_range_sep = false;

            for (size_t j = 0; j < inc; j++) {
                n00b_parse_tree_t *ic = n00b_pt_get_child(item, j);

                if (n00b_pt_is_token(ic)) {
                    const char *tt = n00b_pt_token_text(ic);
                    size_t      tl = n00b_pt_token_text_len(ic);

                    if (tt && tl == 2 && memcmp(tt, "to", 2) == 0) {
                        has_range_sep = true;
                    }
                }
                else if (n_exprs < 2) {
                    exprs[n_exprs++] = ic;
                }
            }

            if (has_range_sep && n_exprs == 2) {
                // Range: switch_val >= low && switch_val <= high
                n00b_cg_val_t low  = codegen_walk(s, exprs[0]);
                n00b_cg_val_t high = codegen_walk(s, exprs[1]);
                n00b_cg_val_t ge   = n00b_cg_emit_ge(s, switch_val, low);
                n00b_cg_val_t le   = n00b_cg_emit_le(s, switch_val, high);
                n00b_cg_val_t both = n00b_cg_emit_and(s, ge, le);
                n00b_cg_emit_bt(s, both, match_label);
            }
            else if (n_exprs >= 1) {
                // Simple value match.
                n00b_cg_val_t val = codegen_walk(s, exprs[0]);
                n00b_cg_val_t eq  = n00b_cg_emit_eq(s, switch_val, val);
                n00b_cg_emit_bt(s, eq, match_label);
            }
        }
    }

    // No match — jump to next case.
    n00b_cg_emit_jmp(s, next_label);

    // Match: emit body, then jump to end.
    n00b_cg_label_here(s, match_label);

    if (body) {
        codegen_walk(s, body);
    }

    n00b_cg_emit_jmp(s, end_label);
}

static void
codegen_switch_branch_list(n00b_cg_session_t *s,
                           n00b_parse_tree_t *branch_list,
                           n00b_cg_val_t      switch_val,
                           n00b_cg_val_t      end_label)
{
    // <branch-list> ::= <eos>* %"case" <switch-case-block>
    // <branch-list> ::= <eos>* %"case" <switch-case-block> <branch-list>
    // Children: some tokens (eos, "case"), then a <switch-case-block>,
    // then optionally another <branch-list>.
    n00b_parse_tree_t *case_block  = NULL;
    n00b_parse_tree_t *next_branch = NULL;
    size_t             nc          = n00b_pt_num_children(branch_list);

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(branch_list, i);

        if (n00b_pt_is_token(child) || n00b_pt_is_group(child)) {
            continue;
        }

        if (!case_block) {
            case_block = child;
        }
        else {
            next_branch = child;
            break;
        }
    }

    n00b_cg_val_t next_label = n00b_cg_label_new(s);

    if (case_block) {
        codegen_switch_case_block(s, case_block, switch_val, end_label, next_label);
    }

    n00b_cg_label_here(s, next_label);

    if (next_branch) {
        codegen_switch_branch_list(s, next_branch, switch_val, end_label);
    }
}

static n00b_cg_val_t
codegen_switch(n00b_cg_session_t *s, n00b_cf_label_t *cf)
{
    // @switch($1, $3): cond = switch expression, then_body = <switch-cases>
    n00b_cg_val_t switch_val = N00B_CG_VOID_VAL;

    if (cf->cond) {
        switch_val = codegen_walk(s, cf->cond);
    }

    n00b_cg_val_t end_label = n00b_cg_label_new(s);

    if (cf->then_body) {
        // <switch-cases> ::= <branch-list> <eos>* <case-else>?
        n00b_parse_tree_t *branch_list = NULL;
        n00b_parse_tree_t *case_else   = NULL;
        size_t             nc          = n00b_pt_num_children(cf->then_body);

        for (size_t i = 0; i < nc; i++) {
            n00b_parse_tree_t *child = n00b_pt_get_child(cf->then_body, i);

            if (n00b_pt_is_token(child) || n00b_pt_is_group(child)) {
                continue;
            }

            // First non-token is branch-list, second (if any) is case-else.
            if (!branch_list) {
                branch_list = child;
            }
            else {
                case_else = child;
                break;
            }
        }

        if (branch_list) {
            codegen_switch_branch_list(s, branch_list, switch_val, end_label);
        }

        if (case_else) {
            // <case-else> ::= %"else" <case-body> | %"else" <body>
            // Walk children, emit the body.
            size_t enc = n00b_pt_num_children(case_else);

            for (size_t i = 0; i < enc; i++) {
                n00b_parse_tree_t *child = n00b_pt_get_child(case_else, i);

                if (!n00b_pt_is_token(child)) {
                    codegen_walk(s, child);
                    break;
                }
            }
        }
    }

    n00b_cg_label_here(s, end_label);

    return N00B_CG_VOID_VAL;
}

// ============================================================================
// Named tuple handler
// ============================================================================

static n00b_cg_val_t
codegen_tuple(n00b_cg_session_t *s, n00b_parse_tree_t *node)
{
    // Check if this is a named tuple (has named-field-list child)
    // or a parenthesized expression (just walk children).
    n00b_parse_tree_t *field_list = NULL;
    size_t nc = n00b_pt_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(node, i);

        if (child && n00b_pt_is_nt(child, "named-field-list")) {
            field_list = child;
            break;
        }
    }

    if (!field_list) {
        // Parenthesized expression or positional tuple — walk children.
        return codegen_children_default(s, node);
    }

    // Named tuple: collect field names and values via DFS through
    // named-field-list -> named-field nodes.
    const char    *names[32];
    n00b_cg_val_t  values[32];
    int32_t        n_fields = 0;

    n00b_parse_tree_t *stack[64];
    int                sp = 0;

    stack[sp++] = field_list;

    while (sp > 0 && n_fields < 32) {
        n00b_parse_tree_t *cur = stack[--sp];

        if (!cur || n00b_pt_is_token(cur)) {
            continue;
        }

        if (n00b_pt_is_nt(cur, "named-field")) {
            // named-field: IDENTIFIER ":" expression
            const char *fname = NULL;
            size_t fname_len = 0;
            n00b_cg_val_t fval = N00B_CG_VOID_VAL;

            size_t fnc = n00b_pt_num_children(cur);
            bool past_colon = false;

            for (size_t j = 0; j < fnc; j++) {
                n00b_parse_tree_t *fc = n00b_pt_get_child(cur, j);

                if (n00b_pt_is_token(fc)) {
                    const char *t = n00b_pt_token_text(fc);
                    size_t tl = n00b_pt_token_text_len(fc);

                    if (tl == 1 && t[0] == ':') {
                        past_colon = true;
                    }
                    else if (!past_colon && !fname) {
                        fname = t;
                        fname_len = tl;
                    }
                }
                else if (past_colon) {
                    fval = codegen_walk(s, fc);
                }
            }

            if (fname && fname_len > 0 && fval.kind != N00B_CG_VAL_VOID) {
                char *name_buf = n00b_alloc_size(1, fname_len + 1);
                memcpy(name_buf, fname, fname_len);
                name_buf[fname_len] = '\0';
                names[n_fields] = name_buf;
                values[n_fields] = fval;
                n_fields++;
            }

            continue;
        }

        // Push children in reverse for L-to-R order.
        size_t cnc = n00b_pt_num_children(cur);

        for (size_t i = cnc; i > 0; i--) {
            if (sp < 64) {
                stack[sp++] = n00b_pt_get_child(cur, i - 1);
            }
        }
    }

    if (n_fields == 0) {
        return N00B_CG_VOID_VAL;
    }

    // Look up the structural tuple type created by the annotation walk's
    // @record handler. Build the same name it used.
    size_t name_len = 7; // "$$tuple"

    for (int32_t i = 0; i < n_fields; i++) {
        name_len += 1 + strlen(names[i]); // "$" + name
    }

    char *tuple_name = n00b_alloc_size(1, name_len + 1);
    char *p = tuple_name;

    memcpy(p, "$$tuple", 7);
    p += 7;

    for (int32_t i = 0; i < n_fields; i++) {
        *p++ = '$';
        size_t fl = strlen(names[i]);
        memcpy(p, names[i], fl);
        p += fl;
    }

    *p = '\0';

    n00b_annot_result_t *ar = current_annot(s);
    n00b_sym_entry_t *tuple_sym = NULL;

    if (ar && ar->symtab) {
        n00b_string_t *tname = n00b_string_from_cstr(tuple_name);
        tuple_sym = n00b_symtab_lookup_any(ar->symtab,
                                            n00b_string_empty(), tname);

        // Compute field layout if not done yet.
        if (tuple_sym && tuple_sym->exposed_scope && !tuple_sym->class_layout) {
            tuple_sym->class_layout = compute_class_layout(s,
                                          tuple_sym->exposed_scope);
        }
    }

    n00b_class_layout_t *layout = tuple_sym ? tuple_sym->class_layout : NULL;
    int64_t instance_size = layout ? (int64_t)layout->instance_size
                                   : (int64_t)(n_fields * 8);

    n00b_cg_type_tag_t alloc_pt[] = {N00B_CG_I64};
    n00b_cg_import_func(s, "n00b_builtin_obj_alloc",
                         (void *)n00b_builtin_obj_alloc,
                         .ret = N00B_CG_I64,
                         .param_types = alloc_pt, .n_params = 1);
    n00b_cg_val_t size_arg = _n00b_cg_const_i64(s, instance_size);
    n00b_cg_val_t obj = n00b_cg_emit_call(
        s, "n00b_builtin_obj_alloc", &size_arg, 1, .ret = N00B_CG_I64);
    obj.type_tag = N00B_CG_PTR;

    n00b_cg_type_tag_t set_pt[] = {N00B_CG_I64, N00B_CG_I64, N00B_CG_I64};
    n00b_cg_import_func(s, "n00b_builtin_field_set",
                         (void *)n00b_builtin_field_set,
                         .ret = N00B_CG_VOID,
                         .param_types = set_pt, .n_params = 3);

    for (int32_t i = 0; i < n_fields; i++) {
        int64_t offset = layout ? (int64_t)layout->field_offsets[i]
                                : (int64_t)(i * 8);
        n00b_cg_val_t offset_arg = _n00b_cg_const_i64(s, offset);
        n00b_cg_val_t set_args[] = {obj, offset_arg, values[i]};
        n00b_cg_emit_call(s, "n00b_builtin_field_set",
                           set_args, 3, .ret = N00B_CG_VOID);
    }

    return obj;
}

// ============================================================================
// Class declaration handler
// ============================================================================

// Compute field layout for a class from its exposed scope.
// All fields are 8 bytes (uint64_t) for the interpreter.
static n00b_class_layout_t *
compute_class_layout(n00b_cg_session_t *s, n00b_scope_t *scope)
{
    // Count fields. Skip type entries, and the class's own var entry
    // (which has exposed_scope set or matches the scope name).
    int32_t n_fields = 0;

    for (n00b_sym_entry_t *e = scope->first_in_scope; e; e = e->next_in_scope) {
        if (e->kind != N00B_SYM_VARIABLE && e->kind != N00B_SYM_PARAM) {
            continue;
        }

        // Skip the class's own entry (has exposed_scope or name matches scope).
        if (e->exposed_scope) {
            continue;
        }

        if (scope->name && e->name
            && e->name->u8_bytes == scope->name->u8_bytes
            && memcmp(e->name->data, scope->name->data,
                      e->name->u8_bytes) == 0) {
            continue;
        }

        n_fields++;
    }

    n00b_class_layout_t *layout = n00b_alloc(n00b_class_layout_t);
    layout->n_fields      = (uint32_t)n_fields;
    layout->field_names   = n00b_alloc_array(const char *, (size_t)n_fields);
    layout->field_offsets  = n00b_alloc_array(uint32_t, (size_t)n_fields);

    // Collect fields into a temp array so we can reverse the scope chain
    // order (scope chain is newest-first, we want declaration order).
    n00b_sym_entry_t *field_entries[n_fields];
    int32_t fe_count = 0;

    for (n00b_sym_entry_t *e = scope->first_in_scope; e; e = e->next_in_scope) {
        if (e->kind != N00B_SYM_VARIABLE && e->kind != N00B_SYM_PARAM) {
            continue;
        }

        if (e->exposed_scope) {
            continue;
        }

        if (scope->name && e->name
            && e->name->u8_bytes == scope->name->u8_bytes
            && memcmp(e->name->data, scope->name->data,
                      e->name->u8_bytes) == 0) {
            continue;
        }

        field_entries[fe_count++] = e;
    }

    // Reverse to get declaration order.
    for (int32_t i = 0; i < fe_count / 2; i++) {
        n00b_sym_entry_t *tmp = field_entries[i];
        field_entries[i] = field_entries[fe_count - 1 - i];
        field_entries[fe_count - 1 - i] = tmp;
    }

    // All fields are 8 bytes, sequentially laid out.
    for (int32_t idx = 0; idx < fe_count; idx++) {
        n00b_sym_entry_t *e = field_entries[idx];
        char *name = n00b_alloc_size(1, e->name->u8_bytes + 1);
        memcpy(name, e->name->data, e->name->u8_bytes);
        name[e->name->u8_bytes] = '\0';

        layout->field_names[idx]  = name;
        layout->field_offsets[idx] = (uint32_t)(idx * 8);
    }

    layout->instance_size = (uint32_t)(n_fields * 8);

    // Collect methods (N00B_SYM_FUNCTION with is_method).
    int32_t n_methods = 0;

    for (n00b_sym_entry_t *e = scope->first_in_scope; e; e = e->next_in_scope) {
        if (e->kind == N00B_SYM_FUNCTION && e->is_method) {
            n_methods++;
        }
    }

    if (n_methods > 0) {
        layout->n_methods        = (uint32_t)n_methods;
        layout->method_names     = n00b_alloc_array(const char *, (size_t)n_methods);
        layout->method_mir_names = n00b_alloc_array(const char *, (size_t)n_methods);

        // Get the class name from the scope name for mangling.
        const char *cname = scope->name ? scope->name->data : "";
        size_t cname_len  = scope->name ? scope->name->u8_bytes : 0;

        int32_t mi = 0;

        for (n00b_sym_entry_t *e = scope->first_in_scope; e; e = e->next_in_scope) {
            if (e->kind != N00B_SYM_FUNCTION || !e->is_method) {
                continue;
            }

            // Unmangled method name.
            char *mname = n00b_alloc_size(1, e->name->u8_bytes + 1);
            memcpy(mname, e->name->data, e->name->u8_bytes);
            mname[e->name->u8_bytes] = '\0';
            layout->method_names[mi] = mname;

            // Mangled MIR name: ClassName$methodName.
            size_t mir_len = cname_len + 1 + e->name->u8_bytes;
            char *mir_name = n00b_alloc_size(1, mir_len + 1);
            memcpy(mir_name, cname, cname_len);
            mir_name[cname_len] = '$';
            memcpy(mir_name + cname_len + 1, e->name->data, e->name->u8_bytes);
            mir_name[mir_len] = '\0';
            layout->method_mir_names[mi] = mir_name;

            if (e->name->u8_bytes == 4
                && memcmp(e->name->data, "init", 4) == 0) {
                layout->has_init = true;
            }

            mi++;
        }
    }

    return layout;
}

// Look up a field index by name in a class layout.
static int32_t
layout_field_index(n00b_class_layout_t *layout, const char *name, size_t name_len)
{
    for (uint32_t i = 0; i < layout->n_fields; i++) {
        if (strlen(layout->field_names[i]) == name_len
            && memcmp(layout->field_names[i], name, name_len) == 0) {
            return (int32_t)i;
        }
    }

    return -1;
}

// C runtime: allocate a class instance (zeroed).
static void *
n00b_builtin_obj_alloc(int64_t size)
{
    return n00b_alloc_size(1, (size_t)size);
}

// C runtime: read a field from an object at a byte offset.
static uint64_t
n00b_builtin_field_get(void *obj, int64_t offset)
{
    return *(uint64_t *)((char *)obj + offset);
}

// C runtime: write a field in an object at a byte offset.
static void
n00b_builtin_field_set(void *obj, int64_t offset, uint64_t value)
{
    *(uint64_t *)((char *)obj + offset) = value;
}

static n00b_cg_val_t
codegen_class_decl(n00b_cg_session_t *s, n00b_parse_tree_t *node)
{
    // Find the class name. The @declares annotation already created
    // a sym entry. The class name is the identifier following "class".
    // Use n00b_pt_find_child_by_nt to find concrete-class or
    // parameterized-class, then extract the first token (the name).
    // Search children manually — n00b_pt_find_child_by_nt may have
    // string encoding issues with ncc-compiled code.
    n00b_parse_tree_t *class_node = NULL;
    size_t nc = n00b_pt_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(node, i);

        if (n00b_pt_is_nt(child, "concrete-class")
            || n00b_pt_is_nt(child, "parameterized-class")) {
            class_node = child;
            break;
        }
    }

    if (!class_node) {
        return N00B_CG_VOID_VAL;
    }

    n00b_parse_tree_t *name_tok = n00b_pt_first_token(class_node);

    if (!name_tok) {
        return N00B_CG_VOID_VAL;
    }

    const char *class_name = n00b_pt_token_text(name_tok);
    size_t      class_len  = n00b_pt_token_text_len(name_tok);

    if (!class_name || class_len == 0) {
        return N00B_CG_VOID_VAL;
    }

    // Look up the class symbol to get its exposed_scope.
    n00b_annot_result_t *ar = current_annot(s);

    if (!ar || !ar->symtab) {
        return N00B_CG_VOID_VAL;
    }

    n00b_string_t *name_str = n00b_string_from_raw(class_name, (int64_t)class_len);
    n00b_sym_entry_t *sym = n00b_symtab_lookup_any(ar->symtab,
                                                     n00b_string_empty(), name_str);

    if (!sym || !sym->exposed_scope) {
        return N00B_CG_VOID_VAL;
    }

    // Compute the field layout.
    n00b_class_layout_t *layout = compute_class_layout(s, sym->exposed_scope);
    sym->class_layout = layout;

    // Back-link from type to sym entry so field access can find the layout.
    // This is a fallback for cases where the type-based lookup doesn't work.
    if (sym->type_var) {
        n00b_tc_type_t *root = n00b_tc_find(sym->type_var);
        root->user_data = sym;
    }

    return N00B_CG_VOID_VAL;
}

// ============================================================================
// Enum statement handler
// ============================================================================

static n00b_cg_val_t
codegen_enum_stmt(n00b_cg_session_t *s, n00b_parse_tree_t *node)
{
    // Grammar: <enum-stmt> ::= <enum-prefix> IDENTIFIER? { <enum-items> }
    // Walk enum-items, assign auto-incrementing values (or explicit ones),
    // and store on the sym_entry's const_value.
    n00b_annot_result_t *ar = current_annot(s);

    if (!ar || !ar->symtab) {
        return N00B_CG_VOID_VAL;
    }

    // Find the enum-items child.
    n00b_parse_tree_t *items_node = NULL;
    size_t nc = n00b_pt_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(node, i);

        if (child && n00b_pt_is_nt(child, "enum-items")) {
            items_node = child;
            break;
        }
    }

    if (!items_node) {
        return N00B_CG_VOID_VAL;
    }

    // DFS through enum-items to find all enum-item nodes.
    // Each enum-item has: IDENTIFIER [enum-value?]
    n00b_parse_tree_t *stack[128];
    int                sp = 0;
    int64_t            next_val = 0;

    stack[sp++] = items_node;

    while (sp > 0) {
        n00b_parse_tree_t *cur = stack[--sp];

        if (!cur || n00b_pt_is_token(cur)) {
            continue;
        }

        if (n00b_pt_is_nt(cur, "enum-item")) {
            // Extract member name (first token).
            n00b_parse_tree_t *name_tok = n00b_pt_first_token(cur);

            if (!name_tok) {
                continue;
            }

            const char *name_raw = n00b_pt_token_text(name_tok);
            size_t      name_len = n00b_pt_token_text_len(name_tok);

            if (!name_raw || name_len == 0) {
                continue;
            }

            // Check for explicit value (enum-value child).
            int64_t val = next_val;
            size_t  item_nc = n00b_pt_num_children(cur);

            for (size_t j = 0; j < item_nc; j++) {
                n00b_parse_tree_t *vc = n00b_pt_get_child(cur, j);

                if (vc && n00b_pt_is_nt(vc, "enum-value")) {
                    // enum-value has: ":" or "=" then a simple-lit.
                    n00b_parse_tree_t *lit_tok = NULL;
                    size_t vnc = n00b_pt_num_children(vc);

                    for (size_t k = 0; k < vnc; k++) {
                        n00b_parse_tree_t *vcc = n00b_pt_get_child(vc, k);

                        if (vcc && n00b_pt_is_token(vcc)) {
                            const char *t = n00b_pt_token_text(vcc);
                            size_t tl = n00b_pt_token_text_len(vcc);

                            // Skip ":" and "=" tokens.
                            if (tl == 1 && (t[0] == ':' || t[0] == '=')) {
                                continue;
                            }

                            lit_tok = vcc;
                            break;
                        }
                        else if (vcc && !n00b_pt_is_token(vcc)) {
                            // simple-lit wrapper — get its token.
                            lit_tok = n00b_pt_first_token(vcc);
                            break;
                        }
                    }

                    if (lit_tok) {
                        const char *vt = n00b_pt_token_text(lit_tok);
                        size_t vtl = n00b_pt_token_text_len(lit_tok);

                        if (vt && vtl > 0) {
                            char vbuf[vtl + 1];
                            memcpy(vbuf, vt, vtl);
                            vbuf[vtl] = '\0';
                            val = strtoll(vbuf, NULL, 0);
                        }
                    }

                    break;
                }
            }

            // Find the symbol entry and set its const_value.
            n00b_string_t *member_name = n00b_string_from_raw(name_raw,
                                                               (int64_t)name_len);
            n00b_sym_entry_t *entry = n00b_symtab_lookup_any(
                ar->symtab, n00b_string_empty(), member_name);

            if (entry) {
                // Store the integer value as a heap-allocated int64_t.
                int64_t *heap_val = n00b_alloc_size(1, sizeof(int64_t));
                *heap_val = val;
                entry->const_value = n00b_option_set(void *, heap_val);
                entry->kind = N00B_SYM_ENUM_CONST;
            }

            next_val = val + 1;
            continue; // Don't recurse into enum-item children.
        }

        // Push children in reverse for left-to-right traversal.
        size_t cnc = n00b_pt_num_children(cur);

        for (size_t i = cnc; i > 0; i--) {
            if (sp < 128) {
                stack[sp++] = n00b_pt_get_child(cur, i - 1);
            }
        }
    }

    return N00B_CG_VOID_VAL;
}

// ============================================================================
// Assert statement handler
// ============================================================================

// Runtime assert failure: called when assert expression is false.
static void
n00b_assert_fail_impl(int64_t line)
{
    if (line > 0) {
        fprintf(stderr, "n00b: assertion failed at line %lld\n",
                (long long)line);
    }
    else {
        fprintf(stderr, "n00b: assertion failed\n");
    }

    abort();
}

static n00b_cg_val_t
codegen_assert_stmt(n00b_cg_session_t *s, n00b_parse_tree_t *node)
{
    // Grammar: <assert-stmt> ::= %"assert" <expression>
    // Evaluate the expression; if false, call abort helper with line number.
    size_t        nc   = n00b_pt_num_children(node);
    n00b_cg_val_t cond = N00B_CG_VOID_VAL;

    // Extract line number from the "assert" keyword token.
    int64_t line = 0;
    n00b_parse_tree_t *first_tok = n00b_pt_first_token(node);

    if (first_tok) {
        n00b_token_info_t *tok = n00b_parse_node_token(first_tok);

        if (tok) {
            line = (int64_t)tok->line;
        }
    }

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(node, i);

        if (!n00b_pt_is_token(child)) {
            cond = codegen_walk(s, child);
            break;
        }
    }

    if (cond.kind == N00B_CG_VAL_VOID) {
        return N00B_CG_VOID_VAL;
    }

    // bt cond, ok_label  (skip abort if condition is true)
    n00b_cg_val_t ok_label = n00b_cg_label_new(s);
    n00b_cg_emit_bt(s, cond, ok_label);

    // Call abort helper with line number.
    n00b_cg_type_tag_t pt[] = {N00B_CG_I64};
    n00b_cg_import_func(s, "n00b_assert_fail",
                          (void *)n00b_assert_fail_impl,
                          .ret = N00B_CG_VOID,
                          .param_types = pt, .n_params = 1);
    n00b_cg_val_t line_arg = _n00b_cg_const_i64(s, line);
    n00b_cg_emit_call(s, "n00b_assert_fail", &line_arg, 1,
                        .ret = N00B_CG_VOID);

    n00b_cg_label_here(s, ok_label);

    return N00B_CG_VOID_VAL;
}

// ============================================================================
// Comptime block handler
// ============================================================================

// Process a comptime body statement: handles assignments and expression
// statements (method calls) using direct C execution.
static n00b_cg_val_t
comptime_walk_stmt(n00b_cg_session_t *s, n00b_parse_tree_t *node)
{
    if (!node || n00b_pt_is_token(node)) {
        return N00B_CG_VOID_VAL;
    }

    n00b_nt_node_t *pn = &n00b_tree_node_value(node);

    // Skip group nodes.
    if (pn->group_top) {
        n00b_cg_val_t result = N00B_CG_VOID_VAL;
        size_t        nc     = n00b_pt_num_children(node);

        for (size_t i = 0; i < nc; i++) {
            n00b_cg_val_t v = comptime_walk_stmt(s, n00b_pt_get_child(node, i));

            if (v.kind != N00B_CG_VAL_VOID) {
                result = v;
            }
        }

        return result;
    }

    // Check if this is an assign-stmt (CF_ASSIGNS annotation).
    n00b_cf_label_t *cf = n00b_codegen_cf_label(s, node);

    if (cf && cf->kind == N00B_CF_ASSIGNS) {
        // Evaluate the RHS.
        n00b_cg_val_t value = N00B_CG_VOID_VAL;

        if (cf->then_body) {
            value = comptime_walk_stmt(s, cf->then_body);
        }

        // Store in comptime_vars.
        if (cf->cond && value.kind != N00B_CG_VAL_VOID) {
            n00b_parse_tree_t *name_tok = n00b_pt_first_token(cf->cond);

            if (name_tok) {
                const char *raw  = n00b_pt_token_text(name_tok);
                size_t      rlen = n00b_pt_token_text_len(name_tok);

                if (raw && rlen > 0) {
                    char *name = n00b_alloc_size(1, rlen + 1);
                    memcpy(name, raw, rlen);
                    name[rlen] = '\0';

                    if (!s->comptime_vars) {
                        s->comptime_vars = n00b_alloc(n00b_dict_untyped_t);
                        n00b_dict_untyped_init(s->comptime_vars, .hash = n00b_hash_cstring);
                    }

                    // Store the pointer from the IMM value.
                    void *obj = (void *)(uintptr_t)value.aux;
                    n00b_dict_untyped_put(s->comptime_vars, (void *)name, obj);
                }
            }
        }

        return value;
    }

    // Check if this is a call (method call on a comptime var).
    if (cf && cf->kind == N00B_CF_CALL) {
        // Check for method call pattern: postfix-expr with "." child.
        if (cf->cond) {
            // Look for a "." token child in the callee node to detect
            // obj.method() pattern.
            size_t nc      = n00b_pt_num_children(cf->cond);
            bool   has_dot = false;

            for (size_t i = 0; i < nc; i++) {
                n00b_parse_tree_t *ch = n00b_pt_get_child(cf->cond, i);

                if (n00b_pt_is_token(ch)) {
                    const char *tt = n00b_pt_token_text(ch);
                    size_t      tl = n00b_pt_token_text_len(ch);

                    if (tl == 1 && tt[0] == '.') {
                        has_dot = true;
                        break;
                    }
                }
            }

            if (has_dot) {
                // Extract object name (first token) and method name (last token).
                const char *obj_name     = NULL;
                size_t      obj_name_len = 0;
                const char *method_name  = NULL;
                size_t      method_len   = 0;

                for (size_t i = 0; i < nc; i++) {
                    n00b_parse_tree_t *ch = n00b_pt_get_child(cf->cond, i);

                    if (n00b_pt_is_token(ch)) {
                        const char *tt = n00b_pt_token_text(ch);
                        size_t      tl = n00b_pt_token_text_len(ch);

                        if (!tt || tl == 0) {
                            continue;
                        }

                        if (tl == 1 && tt[0] == '.') {
                            continue;
                        }

                        if (!obj_name) {
                            obj_name     = tt;
                            obj_name_len = tl;
                        }
                        else {
                            method_name = tt;
                            method_len  = tl;
                        }
                    }
                    else {
                        // Non-token child: recurse to find tokens.
                        n00b_parse_tree_t *ft = n00b_pt_first_token(ch);

                        if (ft) {
                            const char *tt = n00b_pt_token_text(ft);
                            size_t      tl = n00b_pt_token_text_len(ft);

                            if (tt && tl > 0) {
                                if (!obj_name) {
                                    obj_name     = tt;
                                    obj_name_len = tl;
                                }
                                else {
                                    method_name = tt;
                                    method_len  = tl;
                                }
                            }
                        }
                    }
                }

                if (obj_name && method_name && s->comptime_vars) {
                    // Look up the object.
                    char   nbuf[128];
                    size_t copy = obj_name_len < 127 ? obj_name_len : 127;
                    memcpy(nbuf, obj_name, copy);
                    nbuf[copy] = '\0';

                    bool  found = false;
                    void *obj   = n00b_dict_untyped_get(s->comptime_vars, (void *)nbuf, &found);

                    if (found && obj) {
                        // Look up the method.
                        char   mbuf[128];
                        size_t mcopy = method_len < 127 ? method_len : 127;
                        memcpy(mbuf, method_name, mcopy);
                        mbuf[mcopy] = '\0';

                        uint64_t thash  = n00b_obj_typehash(obj);
                        auto     fn_opt = n00b_type_method_lookup(thash, mbuf);

                        if (n00b_option_is_set(fn_opt)) {
                            // Call the method directly in C.
                            void (*fn)(void *) = (void (*)(void *))n00b_option_get(fn_opt);
                            fn(obj);
                        }
                        else {
                            char errbuf[256];
                            snprintf(errbuf, sizeof(errbuf),
                                     "comptime: no method '%s' on object '%s'",
                                     mbuf, nbuf);
                            codegen_error(s, cf->self, "CG003", errbuf);
                        }
                    }
                    else {
                        char errbuf[256];
                        snprintf(errbuf, sizeof(errbuf),
                                 "comptime: variable '%s' not found", nbuf);
                        codegen_error(s, cf->self, "CG004", errbuf);
                    }
                }

                return N00B_CG_VOID_VAL;
            }
        }
    }

    // Check for embed literal (simple-lit with modifier).
    if (n00b_pt_is_nt(node, "simple-lit")) {
        n00b_parse_tree_t *first = n00b_pt_first_token(node);

        if (first) {
            n00b_token_info_t *tok = n00b_parse_node_token(first);

            if (tok && n00b_option_is_set(tok->modifier)) {
                return codegen_embed(s, node);
            }
        }
    }

    // Default: recurse into children.
    n00b_cg_val_t result = N00B_CG_VOID_VAL;
    size_t        nc2    = n00b_pt_num_children(node);

    for (size_t i = 0; i < nc2; i++) {
        n00b_cg_val_t v = comptime_walk_stmt(s, n00b_pt_get_child(node, i));

        if (v.kind != N00B_CG_VAL_VOID) {
            result = v;
        }
    }

    return result;
}

// Process a comptime block: walk body children with direct C execution.
static void
comptime_process_body(n00b_cg_session_t *s, n00b_parse_tree_t *body)
{
    if (!body) {
        return;
    }

    bool was_comptime = s->in_comptime;
    s->in_comptime    = true;

    size_t nc = n00b_pt_num_children(body);

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(body, i);
        comptime_walk_stmt(s, child);
    }

    s->in_comptime = was_comptime;
}

static n00b_cg_val_t
codegen_comptime(n00b_cg_session_t *s, n00b_parse_tree_t *node)
{
    n00b_cg_module_t *m = s->active_module;

    // In pass 2 (cur_func is set), comptime was already processed in
    // pass 1 — skip it.
    if (m && m->cur_func) {
        return N00B_CG_VOID_VAL;
    }

    // Pass 1 context: process the comptime body directly.
    // Find the <body> child (skip the "comptime" keyword token).
    size_t nc = n00b_pt_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(node, i);

        if (!n00b_pt_is_token(child)) {
            comptime_process_body(s, child);
            break;
        }
    }

    return N00B_CG_VOID_VAL;
}

// ============================================================================
// Function definition handler
// ============================================================================

static n00b_cg_val_t
codegen_func_def(n00b_cg_session_t *s, n00b_parse_tree_t *node)
{
    // Grammar:
    // <func-def> @scope("function", $3) @declares($3, "function") ::=
    //     <func-mod>* <func-kind> IDENTIFIER <param-decl>
    //     <where-clause>? <return-type>? <body>
    //
    // Walk children to extract: name, params, return type, body.
    // Return type is a trailing `-> type` after params/where-clause.
    const char        *func_name     = NULL;
    n00b_parse_tree_t *param_decl    = NULL;
    n00b_parse_tree_t *body_node     = NULL;
    n00b_parse_tree_t *ret_type_node = NULL;
    bool               saw_method_kw = false;

    size_t nc          = n00b_pt_num_children(node);
    bool   saw_func_kw = false;

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_pt_get_child(node, i);

        if (n00b_pt_is_token(child)) {
            const char *tt = n00b_pt_token_text(child);
            size_t      tl = n00b_pt_token_text_len(child);

            if (!tt || tl == 0) {
                continue;
            }

            // Check for "func" or "method" keyword.
            if (tl == 4 && memcmp(tt, "func", 4) == 0) {
                saw_func_kw = true;
                continue;
            }

            if (tl == 6 && memcmp(tt, "method", 6) == 0) {
                saw_func_kw    = true;
                saw_method_kw  = true;
                continue;
            }

            // Skip "->" token (part of return-type, handled as NT).
            if (tl == 2 && memcmp(tt, "->", 2) == 0) {
                continue;
            }

            // The IDENTIFIER after func/method is the function name.
            if (saw_func_kw && !func_name) {
                char *buf = n00b_alloc_size(1, tl + 1);
                memcpy(buf, tt, tl);
                buf[tl]   = '\0';
                func_name = buf;
                continue;
            }
        }
        else if (n00b_pt_is_group(child)) {
            // Group nodes from quantifiers — recurse through them.
            // func-mod* and others may create groups.
            continue;
        }
        else {
            // Non-terminal children: func-mod, func-kind, param-decl,
            // where-clause, return-type, or body.

            // Before the func keyword: func-mod or func-kind.
            if (!saw_func_kw) {
                // <func-kind> is "func" | "method" — check first token.
                n00b_parse_tree_t *ft = n00b_pt_first_token(child);

                if (ft) {
                    const char *ftt = n00b_pt_token_text(ft);
                    size_t      ftl = n00b_pt_token_text_len(ft);

                    if (ftt && ftl == 4 && memcmp(ftt, "func", 4) == 0) {
                        saw_func_kw = true;
                        continue;
                    }

                    if (ftt && ftl == 6 && memcmp(ftt, "method", 6) == 0) {
                        saw_func_kw   = true;
                        saw_method_kw = true;
                        continue;
                    }
                }
                // func-mod (private, once) — skip.
                continue;
            }

            if (!func_name) {
                // Check for <method-name> NT: ClassName.methodName
                if (n00b_pt_is_nt(child, "method-name")) {
                    // Extract class name and method name from the NT.
                    const char *mn_class = NULL;
                    size_t      mn_class_len = 0;
                    const char *mn_method = NULL;
                    size_t      mn_method_len = 0;

                    size_t mnc = n00b_pt_num_children(child);

                    for (size_t mi = 0; mi < mnc; mi++) {
                        n00b_parse_tree_t *mc = n00b_pt_get_child(child, mi);

                        if (!n00b_pt_is_token(mc)) {
                            continue;
                        }

                        const char *mt = n00b_pt_token_text(mc);
                        size_t mtl = n00b_pt_token_text_len(mc);

                        if (mtl == 1 && mt[0] == '.') {
                            continue;
                        }

                        if (!mn_class) {
                            mn_class = mt;
                            mn_class_len = mtl;
                        }
                        else if (!mn_method) {
                            mn_method = mt;
                            mn_method_len = mtl;
                        }
                    }

                    if (mn_class && mn_method) {
                        // Build mangled name: ClassName$methodName
                        size_t mlen = mn_class_len + 1 + mn_method_len;
                        char *mbuf = n00b_alloc_size(1, mlen + 1);
                        memcpy(mbuf, mn_class, mn_class_len);
                        mbuf[mn_class_len] = '$';
                        memcpy(mbuf + mn_class_len + 1, mn_method, mn_method_len);
                        mbuf[mlen] = '\0';
                        func_name = mbuf;
                    }
                }

                continue;
            }

            // After the name: param-decl, where-clause, return-type, body.
            // param-decl is first, body is always last.
            // Collect all NT children after name; last one is body,
            // and if there's a return-type (contains "->") we capture it.
            if (!param_decl) {
                param_decl = child;
            }
            else {
                // Check if this NT is a return-type by looking for "->".
                n00b_parse_tree_t *ft     = n00b_pt_first_token(child);
                bool               is_ret = false;

                if (ft) {
                    const char *ftt = n00b_pt_token_text(ft);
                    size_t      ftl = n00b_pt_token_text_len(ft);

                    if (ftt && ftl == 2 && memcmp(ftt, "->", 2) == 0) {
                        is_ret = true;
                    }
                }

                if (is_ret) {
                    ret_type_node = child;
                }
                else {
                    // where-clause or body — body is always last.
                    body_node = child;
                }
            }
        }
    }

    if (!func_name) {
        return codegen_children_default(s, node);
    }

    // Extract parameter names and types from <param-decl>.
    // <param-decl> ::= %"(" <formals>? %")"
    // <formals> ::= <formal-param> ("," <formal-param>)* ...
    // <formal-param> ::= <pos-param> | <k-param>
    // <pos-param> ::= %IDENTIFIER | %IDENTIFIER ":" <type-spec>
    // <k-param>   ::= %IDENTIFIER "=" <expression> | ...
    const char        *param_names[32];
    n00b_cg_type_tag_t param_types[32];
    int32_t            n_params = 0;

    if (param_decl) {
        // Find the formal-param NT id so we can identify them.
        int64_t fp_nt_id = -1;

        if (s->grammar) {
            n00b_string_t *fp_name  = n00b_string_from_cstr("formal-param");
            bool           fp_found = false;

            fp_nt_id = n00b_dict_get(s->grammar->nt_map, fp_name, &fp_found);

            if (!fp_found) {
                fp_nt_id = -1;
            }
        }

        // DFS through param-decl to find all formal-param nodes.
        n00b_parse_tree_t *fp_stack[128];
        int                fp_sp = 0;

        fp_stack[fp_sp++] = param_decl;

        while (fp_sp > 0 && n_params < 32) {
            n00b_parse_tree_t *cur = fp_stack[--fp_sp];

            if (!cur || n00b_pt_is_token(cur)) {
                continue;
            }

            n00b_nt_node_t *cpn = &n00b_tree_node_value(cur);

            if (cpn->id == fp_nt_id) {
                // Found a formal-param. Extract name from first token.
                n00b_parse_tree_t *pt = n00b_pt_first_token(cur);

                if (pt) {
                    const char *pn = n00b_pt_token_text(pt);
                    size_t      pl = n00b_pt_token_text_len(pt);

                    if (pn && pl > 0) {
                        char *pbuf = n00b_alloc_size(1, pl + 1);
                        memcpy(pbuf, pn, pl);
                        pbuf[pl]              = '\0';
                        param_names[n_params] = pbuf;
                        param_types[n_params] = n00b_codegen_node_type(s, cur);
                        n_params++;
                    }
                }

                continue; // Don't recurse into formal-param.
            }

            // Push children in reverse for left-to-right visit.
            size_t cnc = n00b_pt_num_children(cur);

            for (size_t ci = cnc; ci > 0; ci--) {
                if (fp_sp < 128) {
                    fp_stack[fp_sp++] = n00b_pt_get_child(cur, ci - 1);
                }
            }
        }
    }

    // Determine return type.
    n00b_cg_type_tag_t ret_type = N00B_CG_I64;

    if (ret_type_node) {
        ret_type = n00b_codegen_node_type(s, ret_type_node);
    }
    else {
        // Try to get from the func-def node's own type.
        ret_type = n00b_codegen_node_type(s, node);
    }

    // Function definitions must be emitted as top-level MIR functions.
    // If we're inside a wrapper function (like _eval in the REPL),
    // we can't nest MIR functions. Instead, we use the deferred
    // approach: the func-def is emitted *outside* the wrapper by
    // n00b_cg_emit_func_from_tree's pre-scan pass. When we reach
    // here during normal tree-walk, the function was already emitted,
    // so we just return void.
    //
    // If we're NOT inside a wrapper (cur_func == NULL), emit directly.
    n00b_cg_module_t *m = s->active_module;

    if (m->cur_func != NULL) {
        // Inside a wrapper — func was already emitted in the pre-scan.
        return N00B_CG_VOID_VAL;
    }

    // For bare method declarations (method keyword, no dotted name),
    // look up the mangled name from the annotation walk's symtab.
    if (saw_method_kw && func_name && !strchr(func_name, '$')) {
        n00b_annot_result_t *ar = current_annot(s);

        if (ar && ar->symtab) {
            // The annotation walk created a mangled entry ClassName$methodName.
            // Look it up by scanning all scopes for a matching method sym.
            size_t fn_len = strlen(func_name);
            n00b_namespace_t *ns = &ar->symtab->namespaces[0]; // default ns

            for (int32_t si = 0; si < ns->all_count; si++) {
                n00b_scope_t *sc = ns->all_scopes[si];

                for (n00b_sym_entry_t *e = sc->first_in_scope; e; e = e->next_in_scope) {
                    if (e->kind == N00B_SYM_FUNCTION && e->is_method
                        && e->name && e->name->u8_bytes > fn_len + 1) {
                        const char *d = memchr(e->name->data, '$', e->name->u8_bytes);

                        if (d && (size_t)(e->name->u8_bytes - (d - e->name->data) - 1) == fn_len
                            && memcmp(d + 1, func_name, fn_len) == 0) {
                            char *mbuf = n00b_alloc_size(1, e->name->u8_bytes + 1);
                            memcpy(mbuf, e->name->data, e->name->u8_bytes);
                            mbuf[e->name->u8_bytes] = '\0';
                            func_name = mbuf;
                            goto found_mangled;
                        }
                    }
                }
            }
            found_mangled:;
        }
    }

    // Check if this is a method (mangled name contains '$').
    bool is_method_def = false;

    for (const char *p = func_name; *p; p++) {
        if (*p == '$') {
            is_method_def = true;
            break;
        }
    }

    // For methods, prepend implicit `self` parameter.
    if (is_method_def && n_params < 31) {
        for (int32_t i = n_params; i > 0; i--) {
            param_names[i] = param_names[i - 1];
            param_types[i] = param_types[i - 1];
        }

        param_names[0] = "self";
        param_types[0] = N00B_CG_I64; // Pointer to instance.
        n_params++;
    }

    // Top-level emit: not inside any function.
    n00b_cg_begin_func(s,
                       func_name,
                       .ret         = ret_type,
                       .param_names = (const char **)param_names,
                       .param_types = param_types,
                       .n_params    = n_params);

    n00b_cg_val_t body_result = N00B_CG_VOID_VAL;

    if (body_node) {
        body_result = codegen_walk(s, body_node);
    }

    if (body_result.kind != N00B_CG_VAL_VOID) {
        n00b_cg_emit_ret(s, body_result);
    }
    else if (ret_type == N00B_CG_VOID) {
        n00b_cg_emit_ret_void(s);
    }
    else {
        n00b_cg_emit_ret(s, _n00b_cg_const_i64(s, 0));
    }

    n00b_cg_end_func(s);

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

    // Leaf token: return VOID.
    // Literal tokens are handled by @literal annotations on their parent
    // node (codegen_literal), and operator tokens are extracted by
    // codegen_operator. Bare tokens (keywords, punctuation, newlines)
    // should not produce values.
    if (n00b_parse_node_is_token(node)) {
        return N00B_CG_VOID_VAL;
    }

    n00b_nt_node_t *pn = &n00b_tree_node_value(node);

    // Skip group nodes: recurse through them.
    if (pn->group_top) {
        return codegen_children_default(s, node);
    }

    // Check for explicit handler.
    if (s->handlers && pn->id >= 0 && pn->id < s->handler_cap && s->handlers[pn->id]) {
        return s->handlers[pn->id](s, node);
    }

    // Check CF labels.
    n00b_cf_label_t *cf = n00b_codegen_cf_label(s, node);

    if (cf) {
        switch (cf->kind) {
        case N00B_CF_BRANCH:
            return codegen_branch(s, cf);
        case N00B_CF_LOOP:
            return codegen_loop(s, cf);
        case N00B_CF_JUMP:
            return codegen_jump(s, cf);
        case N00B_CF_ASSIGNS:
            return codegen_assign(s, cf);
        case N00B_CF_VARREF:
            return codegen_varref(s, cf);
        case N00B_CF_SWITCH:
            return codegen_switch(s, cf);
        case N00B_CF_UNWRAP_RESULT:
            return codegen_unwrap_result(s, cf);
        case N00B_CF_CALL:
            return codegen_call_cf(s, cf);
        case N00B_CF_CAPTURE:
            break;
        }
    }

    // Member access: expr.name
    // Handles: enum constants (Color.Red), class field access (p.x).
    if (n00b_pt_is_nt(node, "postfix-expr")) {
        size_t mnc = n00b_pt_num_children(node);
        bool has_dot = false;
        n00b_parse_tree_t *lhs_node = NULL;
        const char *rhs_name = NULL;
        size_t rhs_len = 0;

        for (size_t i = 0; i < mnc; i++) {
            n00b_parse_tree_t *child = n00b_pt_get_child(node, i);

            if (n00b_pt_is_token(child)) {
                const char *t = n00b_pt_token_text(child);
                size_t tl = n00b_pt_token_text_len(child);

                if (tl == 1 && t[0] == '.') {
                    has_dot = true;
                }
                else if (has_dot && tl > 0) {
                    rhs_name = t;
                    rhs_len = tl;
                }
            }
            else if (!has_dot) {
                lhs_node = child;
            }
        }

        if (has_dot && lhs_node && rhs_name && rhs_len > 0) {
            n00b_annot_result_t *ar = current_annot(s);

            if (ar && ar->symtab) {
                // First try: LHS is a type/enum name with exposed_scope
                // (e.g., Color.Red).
                n00b_parse_tree_t *lhs_tok = n00b_pt_first_token(lhs_node);

                if (lhs_tok) {
                    const char *lhs_name = n00b_pt_token_text(lhs_tok);
                    size_t lhs_len = n00b_pt_token_text_len(lhs_tok);

                    if (lhs_name && lhs_len > 0) {
                        n00b_string_t *lhs_str = n00b_string_from_raw(
                            lhs_name, (int64_t)lhs_len);
                        n00b_sym_entry_t *sym = n00b_symtab_lookup_any(
                            ar->symtab, n00b_string_empty(), lhs_str);

                        if (sym && sym->exposed_scope) {
                            n00b_string_t *rhs_str = n00b_string_from_raw(
                                rhs_name, (int64_t)rhs_len);
                            n00b_sym_entry_t *member = NULL;

                            for (n00b_sym_entry_t *e = sym->exposed_scope->first_in_scope;
                                 e; e = e->next_in_scope) {
                                if (e->name
                                    && e->name->u8_bytes == rhs_str->u8_bytes
                                    && memcmp(e->name->data, rhs_str->data,
                                              rhs_str->u8_bytes) == 0) {
                                    member = e;
                                    break;
                                }
                            }

                            // Enum constant access.
                            if (member && n00b_option_is_set(member->const_value)) {
                                int64_t *valp = n00b_option_get(member->const_value);
                                return _n00b_cg_const_i64(s, *valp);
                            }
                        }
                    }
                }

                // Second try: LHS is an instance variable, RHS is a field.
                // Resolve LHS type via the type system, find the class
                // sym entry by type name, and get the field layout.
                n00b_cg_val_t obj_val = codegen_walk(s, lhs_node);

                if (obj_val.kind != N00B_CG_VAL_VOID) {
                    // Get the LHS type from the annotation result.
                    n00b_tc_type_t *lhs_type = n00b_codegen_node_tc_type(s, lhs_node);
                    n00b_class_layout_t *layout = NULL;
                    n00b_sym_entry_t *type_sym = NULL;

                    if (lhs_type) {
                        // If the type is a named Prim (class name),
                        // look up the class sym entry by name.
                        if (n00b_variant_is_type(lhs_type->kind, n00b_tc_prim_t)) {
                            n00b_tc_prim_t prim = n00b_variant_get(
                                lhs_type->kind, n00b_tc_prim_t);

                            if (prim.name && prim.name->u8_bytes > 0) {
                                type_sym = n00b_symtab_lookup_any(
                                    ar->symtab, n00b_string_empty(),
                                    prim.name);
                            }
                        }
                        else if (n00b_variant_is_type(lhs_type->kind, n00b_tc_record_t)) {
                            n00b_tc_record_t rec = n00b_variant_get(
                                lhs_type->kind, n00b_tc_record_t);

                            if (rec.name && rec.name->u8_bytes > 0) {
                                type_sym = n00b_symtab_lookup_any(
                                    ar->symtab, n00b_string_empty(),
                                    rec.name);
                            }
                        }
                    }

                    // Fallback: look up the variable, check user_data.
                    if (!type_sym) {
                        n00b_parse_tree_t *var_tok = n00b_pt_first_token(lhs_node);

                        if (var_tok) {
                            const char *vn = n00b_pt_token_text(var_tok);
                            size_t vl = n00b_pt_token_text_len(var_tok);

                            if (vn && vl > 0) {
                                n00b_string_t *vstr = n00b_string_from_raw(
                                    vn, (int64_t)vl);
                                n00b_sym_entry_t *var_sym = n00b_symtab_lookup_any(
                                    ar->symtab, n00b_string_empty(), vstr);

                                if (var_sym && var_sym->type_var) {
                                    n00b_tc_type_t *root = n00b_tc_find(var_sym->type_var);

                                    if (root->user_data) {
                                        type_sym = (n00b_sym_entry_t *)root->user_data;
                                    }
                                }
                            }
                        }
                    }

                    // Fallback for `self`: inside a method, derive
                    // the class name from the current function name
                    // (mangled as ClassName$methodName).
                    if (!type_sym) {
                        n00b_parse_tree_t *var_tok = n00b_pt_first_token(lhs_node);

                        if (var_tok) {
                            const char *vn = n00b_pt_token_text(var_tok);
                            size_t vl = n00b_pt_token_text_len(var_tok);

                            if (vn && vl == 4 && memcmp(vn, "self", 4) == 0) {
                                n00b_cg_module_t *m = s->active_module;

                                if (m && m->cur_func) {
                                    const char *fname = m->cur_func->u.func->name;
                                    const char *dollar = strchr(fname, '$');

                                    if (dollar) {
                                        size_t clen = (size_t)(dollar - fname);
                                        n00b_string_t *cname = n00b_string_from_raw(
                                            fname, (int64_t)clen);
                                        type_sym = n00b_symtab_lookup_any(
                                            ar->symtab, n00b_string_empty(),
                                            cname);
                                    }
                                }
                            }
                        }
                    }

                    if (type_sym && type_sym->exposed_scope) {
                        if (!type_sym->class_layout) {
                            type_sym->class_layout = compute_class_layout(
                                s, type_sym->exposed_scope);
                        }
                        layout = type_sym->class_layout;
                    }

                    if (layout) {
                        int32_t fidx = layout_field_index(
                            layout, rhs_name, rhs_len);

                        if (fidx >= 0) {
                            n00b_cg_type_tag_t get_pt[] = {
                                N00B_CG_I64, N00B_CG_I64};
                            n00b_cg_import_func(
                                s, "n00b_builtin_field_get",
                                (void *)n00b_builtin_field_get,
                                .ret = N00B_CG_I64,
                                .param_types = get_pt,
                                .n_params = 2);
                            n00b_cg_val_t offset_arg =
                                _n00b_cg_const_i64(
                                    s,
                                    (int64_t)layout->field_offsets[fidx]);
                            n00b_cg_val_t get_args[] = {
                                obj_val, offset_arg};
                            return n00b_cg_emit_call(
                                s, "n00b_builtin_field_get",
                                get_args, 2,
                                .ret = N00B_CG_I64);
                        }
                    }
                }
            }
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
                n00b_annotation_t *a = n00b_list_get(nt->pending_annotations, i);

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
        size_t nc           = n00b_pt_num_children(node);
        int    n_tok        = 0;
        int    n_nt         = 0;
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

    // Check for embed literal.
    // Embed tokens have tok->modifier set (the 'ffi suffix) or are
    // identifiable by their grammar terminal name "EMBED".
    // We check tok->modifier since that's specific to embed literals.
    if (n00b_pt_is_nt(node, "simple-lit")) {
        n00b_parse_tree_t *first = n00b_pt_first_token(node);

        if (first) {
            n00b_token_info_t *tok = n00b_parse_node_token(first);

            if (tok && n00b_option_is_set(tok->modifier)) {
                return codegen_embed(s, node);
            }
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

    int32_t uh_count = 0;
    int32_t ai_count = 0;
    int32_t ex_count = 0;

    for (int64_t id = 0; id < nt_count; id++) {
        n00b_nonterm_t *nt = n00b_get_nonterm(s->grammar, id);

        if (!nt) {
            continue;
        }

        const char *name = nt->name ? nt->name->data : NULL;

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
                n00b_annotation_t *a = n00b_list_get(nt->pending_annotations, i);

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
                n00b_annotation_t *a = n00b_list_get(nt->pending_annotations, i);

                if (a && !annot_is_format_only(a->kind) && a->kind != N00B_ANNOT_NONE) {
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
        audit->unhandled_nts       = NULL;
        audit->auto_inferred       = NULL;
        audit->explicit_handled    = NULL;
        audit->unhandled_count     = 0;
        audit->auto_inferred_count = 0;
        audit->explicit_count      = 0;
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
        MIR_load_external(s->mir_ctx, m->imports[i].name, m->imports[i].addr);
    }

    if (for_gen) {
        if (!s->gen_inited) {
            MIR_gen_init(s->mir_ctx);
            s->gen_inited = true;
        }
        MIR_link(s->mir_ctx, MIR_set_gen_interface, NULL);
    }
    else {
        MIR_link(s->mir_ctx, MIR_set_interp_interface, NULL);
    }

    m->state = N00B_CG_MOD_LINKED;
}

bool
n00b_codegen_interpret(n00b_cg_session_t *s,
                       const char        *func_name,
                       void              *result,
                       void              *args,
                       int32_t            n_args)
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
    }
    else {
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
                            const char        *func_name) _kargs
{
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

    // Report the type of the last expression to the caller.
    if (kargs->result_type) {
        *kargs->result_type = (result.kind != N00B_CG_VAL_VOID)
                              ? result.type_tag
                              : N00B_CG_VOID;
    }

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
n00b_codegen_eval_tree(n00b_grammar_t *grammar, n00b_parse_tree_t *tree) _kargs
{
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

    n00b_codegen_t *cg
        = n00b_codegen_new(grammar, .annot = kargs->annot, .type_map = kargs->type_map);

    bool emit_ok = n00b_cg_emit_func_from_tree(cg, tree, fname, .ret = N00B_CG_I64);

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

        for (n00b_sym_entry_t *sym = scope->first_in_scope; sym; sym = sym->next_in_scope) {
            if (sym->kind != N00B_SYM_FUNCTION) {
                continue;
            }

            n00b_symtab_add(s->global_scope,
                            n00b_string_empty(),
                            sym->name,
                            N00B_SYM_FUNCTION,
                            sym->decl_node);
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
        n00b_string_t    *sname = n00b_string_from_cstr(name);
        n00b_sym_entry_t *sym
            = n00b_symtab_lookup_any(m->annot->symtab, n00b_string_empty(), sname);

        if (sym) {
            return sym;
        }
    }

    // Fall back to session global scope.
    if (m->session && m->session->global_scope) {
        n00b_string_t *sname = n00b_string_from_cstr(name);

        return n00b_symtab_lookup_any(m->session->global_scope, n00b_string_empty(), sname);
    }

    return NULL;
}

n00b_cg_module_t *
n00b_cg_session_find_module(n00b_cg_session_t *s, const char *fqn)
{
    if (!s || !fqn || !s->module_cache) {
        return NULL;
    }

    bool  found = false;
    void *val   = n00b_dict_untyped_get(s->module_cache, fqn, &found);

    return found ? (n00b_cg_module_t *)val : NULL;
}

// ============================================================================
// Session eval tree (new API)
// ============================================================================

int64_t
n00b_cg_session_eval_tree(n00b_cg_session_t *s, n00b_parse_tree_t *tree) _kargs
{
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
    n00b_cg_type_tag_t last_type = N00B_CG_VOID;
    bool emit_ok = n00b_cg_emit_func_from_tree(s, tree, fname,
                                                 .ret = N00B_CG_I64,
                                                 .result_type = &last_type);

    if (kargs->out_type) {
        *kargs->out_type = last_type;
    }

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
n00b_cg_import_table_add(n00b_cg_import_table_t *table, n00b_cg_import_entry_t entry)
{
    if (table->count >= table->cap) {
        int32_t                 new_cap = table->cap * 2;
        n00b_cg_import_entry_t *new_entries
            = n00b_alloc_array(n00b_cg_import_entry_t, (size_t)new_cap);
        memcpy(new_entries,
               table->entries,
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
n00b_cg_import_table_resolve_types(n00b_cg_import_table_t *table, n00b_tc_ctx_t *ctx)
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

// ============================================================================
// Module-level run: two-pass codegen for whole-file execution
// ============================================================================

int64_t
n00b_cg_session_run_module(n00b_cg_session_t *s, n00b_parse_tree_t *tree) _kargs
{
    n00b_annot_result_t *annot;
    const char          *entry_name;
    bool                *ok;
}
{
    if (kargs->ok) {
        *kargs->ok = false;
    }

    if (!tree || !s) {
        return 0;
    }

    const char *entry = kargs->entry_name ? kargs->entry_name : "_main";

    // Create a new module.
    char mod_name[64];
    snprintf(mod_name, sizeof(mod_name), "mod_%d", s->module_count);

    n00b_cg_module_t *m = n00b_cg_module_new(s, mod_name);

    if (kargs->annot) {
        m->annot = kargs->annot;
    }

    // --- Pass 1: emit top-level function definitions + comptime blocks ---
    // Look up the func-def and comptime-stmt NT ids.
    int64_t func_def_nt_id = -1;
    int64_t comptime_nt_id = -1;

    if (s->grammar) {
        n00b_string_t *fd_name = n00b_string_from_cstr("func-def");
        bool           found   = false;

        func_def_nt_id = n00b_dict_get(s->grammar->nt_map, fd_name, &found);

        if (!found) {
            func_def_nt_id = -1;
        }

        n00b_string_t *ct_name = n00b_string_from_cstr("comptime-stmt");
        found                  = false;

        comptime_nt_id = n00b_dict_get(s->grammar->nt_map, ct_name, &found);

        if (!found) {
            comptime_nt_id = -1;
        }
    }

    // Recursively scan the tree for func-def and comptime-stmt nodes.
    // func-defs are emitted as standalone MIR functions.
    // comptime blocks are executed directly in C (no MIR function open).
    {
        // Use a simple stack-based DFS to find target nodes.
        n00b_parse_tree_t *stack[256];
        int                sp = 0;

        stack[sp++] = tree;

        while (sp > 0) {
            n00b_parse_tree_t *cur = stack[--sp];

            if (!cur || n00b_pt_is_token(cur)) {
                continue;
            }

            n00b_nt_node_t *pn = &n00b_tree_node_value(cur);

            if (pn->id == func_def_nt_id) {
                // Emit this function definition at top level.
                codegen_walk(s, cur);
                continue; // Don't recurse into func-def children.
            }

            if (pn->id == comptime_nt_id) {
                // Process comptime block directly in C.
                codegen_comptime(s, cur);
                continue; // Don't recurse into comptime children.
            }

            // Push children in reverse order so we visit them left-to-right.
            size_t nc = n00b_pt_num_children(cur);

            for (size_t i = nc; i > 0; i--) {
                n00b_parse_tree_t *child = n00b_pt_get_child(cur, i - 1);

                if (sp < 256) {
                    stack[sp++] = child;
                }
            }
        }
    }

    // --- Pass 2: wrap all top-level statements in _main ---
    // codegen_func_def will see cur_func != NULL and return VOID,
    // skipping already-emitted function definitions.
    bool emit_ok = n00b_cg_emit_func_from_tree(s, tree, entry, .ret = N00B_CG_I64);

    if (!emit_ok) {
        // Finish the MIR module to leave the context in a clean state,
        // but don't compile or JIT it.
        if (m->state == N00B_CG_MOD_BUILDING) {
            MIR_finish_module(s->mir_ctx);
            m->state = N00B_CG_MOD_FINISHED;
        }

        return 0;
    }

    // Compile the module and get entry function pointer.
    typedef int64_t (*entry_fn_t)(void);

    entry_fn_t fn = (entry_fn_t)n00b_cg_module_compile(m, entry);

    // Merge public symbols.
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
// AOT compilation: extract machine code without executing
// ============================================================================

n00b_module_code_t *
n00b_cg_session_compile_module(n00b_cg_session_t *s, n00b_parse_tree_t *tree) _kargs
{
    n00b_annot_result_t *annot;
    const char          *entry_name;
}
{
#ifdef _WIN32
    (void)s;
    (void)tree;
    return NULL;
#endif

    if (!tree || !s) {
        return NULL;
    }

    const char *entry = kargs->entry_name ? kargs->entry_name : "_main";

    // Create a new module.
    char mod_name[64];
    snprintf(mod_name, sizeof(mod_name), "mod_%d", s->module_count);

    n00b_cg_module_t *m = n00b_cg_module_new(s, mod_name);

    if (kargs->annot) {
        m->annot = kargs->annot;
    }

    // --- Pass 1: emit top-level function definitions + comptime blocks ---
    int64_t func_def_nt_id  = -1;
    int64_t comptime_nt_id2 = -1;

    if (s->grammar) {
        n00b_string_t *fd_name = n00b_string_from_cstr("func-def");
        bool           found   = false;

        func_def_nt_id = n00b_dict_get(s->grammar->nt_map, fd_name, &found);

        if (!found) {
            func_def_nt_id = -1;
        }

        n00b_string_t *ct_name = n00b_string_from_cstr("comptime-stmt");
        found                  = false;

        comptime_nt_id2 = n00b_dict_get(s->grammar->nt_map, ct_name, &found);

        if (!found) {
            comptime_nt_id2 = -1;
        }
    }

    {
        n00b_parse_tree_t *stack[256];
        int                sp = 0;

        stack[sp++] = tree;

        while (sp > 0) {
            n00b_parse_tree_t *cur = stack[--sp];

            if (!cur || n00b_pt_is_token(cur)) {
                continue;
            }

            n00b_nt_node_t *pn = &n00b_tree_node_value(cur);

            if (pn->id == func_def_nt_id) {
                codegen_walk(s, cur);
                continue;
            }

            if (pn->id == comptime_nt_id2) {
                codegen_comptime(s, cur);
                continue;
            }

            size_t nc = n00b_pt_num_children(cur);

            for (size_t i = nc; i > 0; i--) {
                n00b_parse_tree_t *child = n00b_pt_get_child(cur, i - 1);

                if (sp < 256) {
                    stack[sp++] = child;
                }
            }
        }
    }

    // --- Pass 2: wrap remaining statements in _main ---
    bool emit_ok = n00b_cg_emit_func_from_tree(s, tree, entry, .ret = N00B_CG_I64);

    if (!emit_ok) {
        return NULL;
    }

    // Count functions before linking because MIR_set_gen_interface emits all
    // machine code during MIR_link().
    size_t func_count = 0;

    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, m->mir_module->items); item != NULL;
         item            = DLIST_NEXT(MIR_item_t, item)) {
        if (item->item_type == MIR_func_item) {
            func_count++;
        }
    }

    if (func_count == 0) {
        return NULL;
    }

    size_t *code_lens  = calloc(func_count, sizeof(size_t));
    FILE   *code_trace = n00b_mir_open_code_len_trace();

    // Compile the module (finish, load, link, but don't execute).
    // Pass NULL entry to compile all functions without returning a pointer.
    n00b_cg_module_compile_traced(m, NULL, code_trace);

    if (!n00b_mir_finish_code_len_trace(s->mir_ctx, code_trace, code_lens, func_count)) {
        free(code_lens);
        return NULL;
    }

    // Collect import symbol names and their resolved addresses for
    // relocation scanning.
    size_t       import_count = 0;
    const char **import_names = NULL;
    void       **import_addrs = NULL;

    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, m->mir_module->items); item != NULL;
         item            = DLIST_NEXT(MIR_item_t, item)) {
        if (item->item_type == MIR_import_item) {
            import_count++;
        }
    }

    if (import_count > 0) {
        import_names = calloc(import_count, sizeof(const char *));
        import_addrs = calloc(import_count, sizeof(void *));
        size_t idx   = 0;

        for (MIR_item_t item = DLIST_HEAD(MIR_item_t, m->mir_module->items); item != NULL;
             item            = DLIST_NEXT(MIR_item_t, item)) {
            if (item->item_type == MIR_import_item) {
                import_names[idx] = item->u.import_id;
                import_addrs[idx] = item->addr;
                idx++;
            }
        }
    }

    // Allocate result.
    n00b_module_code_t *result = calloc(1, sizeof(n00b_module_code_t));

    result->module_name = mod_name;
    result->func_count  = func_count;
    result->funcs       = calloc(func_count, sizeof(n00b_func_code_t));

    // Extract machine code and relocations for each generated function.
    size_t fi = 0;

    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, m->mir_module->items); item != NULL;
         item            = DLIST_NEXT(MIR_item_t, item)) {
        if (item->item_type != MIR_func_item) {
            continue;
        }

        size_t     code_len = code_lens[fi];
        MIR_func_t func     = item->u.func;

        if (!func->machine_code || code_len == 0) {
            free(result->funcs);
            free(result);
            free(code_lens);
            free(import_names);
            free(import_addrs);
            return NULL;
        }

        result->funcs[fi].name     = func->name;
        result->funcs[fi].code     = (uint8_t *)func->machine_code;
        result->funcs[fi].code_len = code_len;

        // Scan machine code for references to import addresses.
        // On x86_64/aarch64, external addresses appear as 8-byte
        // absolute values in the code or const pool.
        size_t        max_relocs  = import_count;
        n00b_reloc_t *relocs      = NULL;
        size_t        reloc_count = 0;

        if (import_count > 0 && code_len >= 8) {
            relocs        = calloc(max_relocs, sizeof(n00b_reloc_t));
            uint8_t *code = (uint8_t *)func->machine_code;

            for (size_t off = 0; off <= code_len - 8; off++) {
                uint64_t val;
                memcpy(&val, code + off, 8);

                for (size_t j = 0; j < import_count; j++) {
                    if (val == (uint64_t)(uintptr_t)import_addrs[j]) {
                        // Found a reference to an imported symbol.
                        if (reloc_count >= max_relocs) {
                            max_relocs *= 2;
                            relocs = realloc(relocs, max_relocs * sizeof(n00b_reloc_t));
                        }
                        relocs[reloc_count].sym    = import_names[j];
                        relocs[reloc_count].offset = off;
                        reloc_count++;
                        off += 7; // Skip past this address.
                        break;
                    }
                }
            }
        }

        result->funcs[fi].relocs      = relocs;
        result->funcs[fi].reloc_count = reloc_count;
        fi++;
    }

    // Merge public symbols so subsequent modules can see them.
    n00b_cg_session_merge_module(s, m);

    free(code_lens);
    free(import_names);
    free(import_addrs);

    return result;
}

// Platform-specific section boundary declarations.
#ifdef __APPLE__
extern const n00b_cg_import_entry_t __start_n00b_ffi __asm("section$start$__DATA$n00b_ffi");
extern const n00b_cg_import_entry_t __stop_n00b_ffi __asm("section$end$__DATA$n00b_ffi");
#else
extern const n00b_cg_import_entry_t __start_n00b_ffi __attribute__((weak));
extern const n00b_cg_import_entry_t __stop_n00b_ffi __attribute__((weak));
#endif

n00b_cg_import_table_t *
n00b_cg_collect_exports(void)
{
    const n00b_cg_import_entry_t *start = &__start_n00b_ffi;
    const n00b_cg_import_entry_t *stop  = &__stop_n00b_ffi;

    n00b_cg_import_table_t *table = n00b_cg_import_table_new();

    if (!start || !stop) {
        return table;
    }

    for (const n00b_cg_import_entry_t *e = start; e < stop; e++) {
        if (e->name && e->addr) {
            n00b_cg_import_table_add(table, *e);
        }
    }

    return table;
}
