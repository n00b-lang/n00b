#pragma once

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lex.h"
#include "macros.h"
#include "st.h"
#include "types.h"
#include "branch_symbols.h"
#include "nt_types.h"

typedef struct parser_t     parser_t;
typedef struct strlist_t    strlist_t;
typedef struct save_state_t save_state_t;
typedef struct memo_entry_t memo_entry_t;
typedef tnode_t *(*nt_fn)(parser_t *ctx);

/**
 * @brief Memo table entry for Packrat parsing.
 *
 * Caches the result of parsing a nonterminal at a position.
 * - end_pos == MEMO_EMPTY: not yet computed
 * - end_pos == MEMO_FAIL: parsing failed
 * - end_pos >= 0: parsing succeeded with given result
 */
struct memo_entry_t {
    tnode_t *result;  /**< Parse result (heap copy; nullptr for FAIL/EMPTY) */
    int      end_pos; /**< Token position after match (-1=FAIL, 0=EMPTY) */
};

#define MEMO_EMPTY 0
#define MEMO_FAIL  -1

#if defined(PDEBUG)
typedef struct pdebug_t pdebug_t;

struct pdebug_t {
    pdebug_t *next;
    pdebug_t *prev;
    char     *nt;
};
#endif

struct strlist_t {
    int   num_items;
    char *items[];
};

struct parser_t {
    const ncc_buf_t *input;
    lex_t         *lex;
    const tok_t   *tokens;
    const int      num_tokens;
    tnode_t       *cur_node;
    tnode_t       *recursive_node;
    symtab_t      *symtab;
    label_table_t *label_table;
    label_table_t  func_lt;
    label_table_t *saved_lt;
    int            pos;
    memo_entry_t **memo;      /**< Memo table: memo[pos][nt_id] */
    int            memo_size; /**< Number of token positions in memo */
#if defined(PDEBUG)
    pdebug_t *debug_root;
    pdebug_t *debug_cur;
#endif
};

struct save_state_t {
    tnode_t           *node;
    tnode_t           *recursive;
    int                token_position;
    parse_arena_mark_t arena_mark; /**< Arena position for memory recovery on backtrack */
};

// Globals from parse_support.c (elided_node already declared in types.h)
extern const tok_t eof_token;

// Memo table functions from parse_support.c
extern void memo_init(parser_t *ctx);
extern void memo_free(parser_t *ctx);
extern bool memo_check(parser_t *ctx, nt_type_t nt_id, tnode_t **result);
extern void memo_store(parser_t *ctx, int start_pos, nt_type_t nt_id, tnode_t *result);

// Support functions from parse_support.c
extern tnode_t *node_alloc(parser_t *ctx, char *nt);
extern tok_t   *get_tok(parser_t *ctx);
extern void     parse_advance(parser_t *ctx);
extern void     parse_prime(parser_t *ctx);
extern void     add_kid(tnode_t *parent, tnode_t *kid);
extern void     add_placeholder(tnode_t *node);
extern void     pstate_save(parser_t *ctx, save_state_t *dst);
extern void     pstate_restore(parser_t *ctx, save_state_t *src);
extern tnode_t *evaluate_nonterminal(parser_t *ctx, nt_fn nonterminal);
extern bool     _optional_nt(parser_t *ctx, nt_fn nonterminal_top);
extern bool     _required_nt(parser_t *ctx, nt_fn nonterminal_top);
extern char    *check_strlist_tok(strlist_t *list, const char *text, int len);
extern tnode_t *_optional_keyword(parser_t *ctx, strlist_t *list);
extern tnode_t *_optional_named_identifier(parser_t *ctx, strlist_t *list);
extern tnode_t *_optional_op(parser_t *ctx, char *op);
extern tnode_t *_optional_op_list(parser_t *ctx, strlist_t *list);
extern bool     can_be_left_recursion(tnode_t *node);
extern bool     _direct_self_call(parser_t *ctx);
extern bool     _indirect_self_call(parser_t *ctx);
extern void     _indirect_self_opt(parser_t *ctx);
extern tnode_t *recursion_loop(parser_t *ctx, int n, nt_fn *branches);
extern void     enter_function_scope(parser_t *ctx);
extern void     exit_function_scope(parser_t *ctx);
extern void     enter_block_scope(parser_t *ctx);
extern void     exit_block_scope(parser_t *ctx);

// Terminal matching functions from parse_support.c
extern tnode_t *provided_identifier(parser_t *ctx);
extern tnode_t *constant(parser_t *ctx);
extern tnode_t *string_literal(parser_t *ctx);
extern tnode_t *typedef_name_terminal(parser_t *ctx);
extern tnode_t *non_bracket_token(parser_t *ctx);

// Keyword lists from parse_support.c
extern strlist_t *kw_type_alignment;
extern strlist_t *kw_type_specifier;
extern strlist_t *kw_fn_specifier;
extern strlist_t *kw_once_id;
extern strlist_t *kw_static_assert;
extern strlist_t *kw_storage_class;
extern strlist_t *kw_typeof;
extern strlist_t *kw_typeof_unqual;
extern strlist_t *kw_typeid;
extern strlist_t *kw_typestr;
extern strlist_t *kw_constexpr_paste;
extern strlist_t *kw_type_qualifier;
extern strlist_t *kw_struct_or_union;
extern strlist_t *kw_bitint;
extern strlist_t *kw_static;
extern strlist_t *kw_atomic;
extern strlist_t *kw_enum;
extern strlist_t *kw_sizeof;
extern strlist_t *kw_alignof;
extern strlist_t *kw_countof;
extern strlist_t *kw_generic;
extern strlist_t *kw_default;
extern strlist_t *kw_true_false_nullptr;
extern strlist_t *kw_builtin_va_arg;
extern strlist_t *kw_builtin_types_compatible_p;
extern strlist_t *kw_opaque;
extern strlist_t *kw_package;
extern strlist_t *kw_if;
extern strlist_t *kw_else;
extern strlist_t *kw_switch;
extern strlist_t *kw_case;
extern strlist_t *kw_while;
extern strlist_t *kw_do;
extern strlist_t *kw_for;
extern strlist_t *kw_goto;
extern strlist_t *kw_continue;
extern strlist_t *kw_break;
extern strlist_t *kw_return;
extern strlist_t *kw_keywords;
extern strlist_t *op_unary;
extern strlist_t *op_assign;
// GCC extensions
extern strlist_t *kw_gcc_attribute;
extern strlist_t *kw_gcc_extension;
extern strlist_t *kw_gcc_asm;

// PDEBUG support
#if defined(PDEBUG)
#define pdebug(fmt, ...) fprintf(stderr, fmt __VA_OPT__(, ) __VA_ARGS__)

extern void pdebug_push(parser_t *ctx, char *nt);
extern void pdebug_pop(parser_t *ctx);
extern void pdebug_show(parser_t *ctx);

#define pdebug_leave(result)                            \
    pdebug_show(ctx);                                        \
    pdebug("Leaving: %s; result = %p (cur tok: %s)\n",      \
           __func__,                                         \
           (void *)result,                                   \
           extract((ncc_buf_t *)ctx->input, get_tok(ctx))); \
    pdebug_pop(ctx);
#else
#define pdebug(fmt, ...)
#define pdebug_push(...)
#define pdebug_leave(...)
#endif

// Inline functions
static inline tnode_t *
cur_node(parser_t *ctx)
{
    return ctx->cur_node;
}

static inline st_reg_ctx_t
make_reg_ctx(parser_t *ctx)
{
    return (st_reg_ctx_t){
        .input      = (ncc_buf_t *)ctx->input,
        .lex        = ctx->lex,
        .st         = ctx->symtab,
        .lt         = ctx->label_table,
        .node       = ctx->cur_node,
        .num_tokens = ctx->num_tokens,
        .tokens     = ctx->tokens,
    };
}

// Macro for defining keyword string lists
#define str_list(name, ...)                     \
    strlist_t name##_base = {                   \
        .num_items = NCC_VA_COUNT(__VA_ARGS__), \
        .items     = {                          \
            __VA_ARGS__,                        \
        },                                      \
    };                                          \
    strlist_t *name = &name##_base

// Per-NT error handlers
// When all branches of an NT fail, the parser normally backtracks silently.
// For NTs below a committed parse point (e.g. typeid_atom inside typeid()),
// we want a diagnostic instead. Handlers are fatal: they call ncc_error + exit.
typedef void (*nt_error_fn)(parser_t *ctx, int start_pos);

extern void register_nt_error_handler(nt_type_t nt_id, nt_error_fn handler);
extern void nt_error_init(void);

static inline void
nt_check_error(parser_t *ctx, nt_type_t nt_id, int start_pos, tnode_t *result)
{
    if (result) {
        return;
    }
    extern nt_error_fn nt_error_handlers[];
    nt_error_fn        handler = nt_error_handlers[nt_id];
    if (handler) {
        handler(ctx, start_pos);
    }
}

// Coverage support
#ifdef COVERAGE
extern void coverage_record_branch(const char *branch_name);
#define COVERAGE_RECORD(fn) coverage_record_branch(fn)
#else
#define COVERAGE_RECORD(fn)
#endif

// Branch definition macros
#define named_node(ctx) node_alloc(ctx, (char *)__func__)

#define start_nt()                                      \
    COVERAGE_RECORD(__func__);                          \
    pdebug("Entering: %s (cur tok: %s)\n",                   \
           __func__,                                         \
           extract((ncc_buf_t *)ctx->input, get_tok(ctx))); \
    pdebug_push(ctx, (char *)__func__);                 \
    ctx->cur_node = named_node(ctx)

#define end_nt()                                            \
    pdebug("Successful match of %s.\n", ctx->cur_node->nt); \
    pdebug_leave(ctx->cur_node);                            \
    return ctx->cur_node

#define nt_branch(nt_name, n) \
    tnode_t *                 \
    nt_name##_##n(parser_t *ctx)

// Declarative interface macros
#define optional_nt(nt) _optional_nt(ctx, nt)

#define required_nt(nt)                               \
    {                                                 \
        if (!_required_nt(ctx, nt)) {                 \
            pdebug("Non-terminal wasn't a match.\n"); \
            pdebug_leave(nullptr);                    \
            return nullptr;                           \
        }                                             \
    }

#define optional_keyword(list) _optional_keyword(ctx, list)

#define required_keyword(list)                            \
    {                                                     \
        if (!_optional_keyword(ctx, list)) {              \
            pdebug("Failed to match required keyword\n"); \
            pdebug_leave(nullptr);                        \
            return nullptr;                               \
        }                                                 \
    }

#define optional_named_identifier(list) _optional_named_identifier(ctx, list)

#define required_named_identifier(list)                            \
    {                                                              \
        if (!_optional_named_identifier(ctx, list)) {              \
            pdebug("Failed to match required named identifier\n"); \
            pdebug_leave(nullptr);                                 \
            return nullptr;                                        \
        }                                                          \
    }

#define optional_op(op) _optional_op(ctx, op)

#define required_op(op)                              \
    {                                                \
        if (!_optional_op(ctx, op)) {                \
            pdebug("Failed to match required op\n"); \
            pdebug_leave(nullptr);                   \
            return nullptr;                          \
        }                                            \
    }

#define optional_op_list(list) _optional_op_list(ctx, list)

#define required_op_list(list)                            \
    {                                                     \
        if (!_optional_op_list(ctx, list)) {              \
            pdebug("Failed to match required op list\n"); \
            pdebug_leave(nullptr);                        \
            return nullptr;                               \
        }                                                 \
    }

// Symbol table registration macros
#define register_declaration(ctx)             \
    do {                                      \
        st_reg_ctx_t _rc = make_reg_ctx(ctx); \
        st_register_declaration(&_rc);        \
    } while (0)

#define register_function_definition(ctx)      \
    do {                                       \
        enter_function_scope(ctx);             \
        st_reg_ctx_t _rc = make_reg_ctx(ctx);  \
        st_register_function_definition(&_rc); \
    } while (0)

#define end_func_scope()                                    \
    exit_function_scope(ctx);                               \
    do {                                                    \
        st_reg_ctx_t _rc = make_reg_ctx(ctx);               \
        st_register_function_name(&_rc);                    \
    } while (0);                                            \
    pdebug("Successful match of %s.\n", ctx->cur_node->nt); \
    pdebug_leave(ctx->cur_node);                            \
    return ctx->cur_node

#define end_block_scope()                                   \
    exit_block_scope(ctx);                                  \
    pdebug("Successful match of %s.\n", ctx->cur_node->nt); \
    pdebug_leave(ctx->cur_node);                            \
    return ctx->cur_node

#define register_package(ctx)                 \
    do {                                      \
        st_reg_ctx_t _rc = make_reg_ctx(ctx); \
        st_register_package(&_rc);            \
    } while (0)

#define register_label_def(ctx)               \
    do {                                      \
        st_reg_ctx_t _rc = make_reg_ctx(ctx); \
        st_register_label_def(&_rc);          \
    } while (0)

#define register_label_ref(ctx)               \
    do {                                      \
        st_reg_ctx_t _rc = make_reg_ctx(ctx); \
        st_register_label_ref(&_rc);          \
    } while (0)

// Left recursion macros
#define direct_self_call(fn)            \
    {                                   \
        if (!_direct_self_call(ctx)) {  \
            pdebug("Not recursive.\n"); \
            pdebug_leave(nullptr);      \
            return nullptr;             \
        }                               \
    }

#define base_case_only()                                         \
    {                                                            \
        if (ctx->recursive_node) {                               \
            pdebug("Has recursive node, skipping base case.\n"); \
            pdebug_leave(nullptr);                               \
            return nullptr;                                      \
        }                                                        \
    }

#define indirect_self_call(...) _indirect_self_call(ctx)
#define indirect_self_opt(...)  _indirect_self_opt(ctx)

// Branch list macros for declare_nt/declare_recursive
#define branch_list_1  0
#define branch_list_2  0, 1
#define branch_list_3  0, 1, 2
#define branch_list_4  0, 1, 2, 3
#define branch_list_5  0, 1, 2, 3, 4
#define branch_list_6  0, 1, 2, 3, 4, 5
#define branch_list_7  0, 1, 2, 3, 4, 5, 6
#define branch_list_8  0, 1, 2, 3, 4, 5, 6, 7
#define branch_list_9  0, 1, 2, 3, 4, 5, 6, 7, 8
#define branch_list_10 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
#define branch_list_11 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10

#define build_one_branch_call(base_name, n)     \
                                                \
    pstate_save(ctx, &state);                   \
    result = NCC_CONCAT3(base_name, _, n)(ctx); \
    if (result) {                               \
        result->branch = n;                     \
        ctx->cur_node  = state.node;            \
        goto nt_done;                           \
    }                                           \
    else {                                      \
        pstate_restore(ctx, &state);            \
    }

#define build_one_branch_arr_item(base_name, n) \
    (nt_fn) NCC_CONCAT3(base_name, _, n),

#define build_one_branch_proto(base_name, n) \
    tnode_t *NCC_CONCAT3(base_name, _, n)(parser_t *);

#define build_branch_calls(base_name, num_branches) \
    NCC_MAP_STATE(build_one_branch_call,            \
                  base_name,                        \
                  branch_list##_##num_branches)

#define array_body(base_name, num_branches)  \
    NCC_MAP_STATE(build_one_branch_arr_item, \
                  base_name,                 \
                  branch_list##_##num_branches)

#define build_branch_array(base_name, num_branches) \
    {array_body(base_name, num_branches) nullptr}

#define build_branch_protos(base_name, num_branches) \
    NCC_MAP_STATE(build_one_branch_proto,            \
                  base_name,                         \
                  branch_list##_##num_branches)

#define declare_nt(base_name, num_branches)                   \
    build_branch_protos(base_name, num_branches);             \
                                                              \
    tnode_t *                                                 \
    base_name(parser_t *ctx)                                  \
    {                                                         \
        static nt_type_t _nt_id = NT_NONE;                    \
        if (_nt_id == NT_NONE) {                              \
            _nt_id = nt_lookup(__func__);                     \
        }                                                     \
                                                              \
        tnode_t *_memo_result;                                \
        if (memo_check(ctx, _nt_id, &_memo_result)) {         \
            return _memo_result;                              \
        }                                                     \
                                                              \
        int          _start_pos = ctx->pos;                   \
        tnode_t     *result     = nullptr;                    \
        save_state_t state;                                   \
        build_branch_calls(base_name, num_branches);          \
        nt_check_error(ctx, _nt_id, _start_pos, result);      \
nt_done:                                                      \
        memo_store(ctx, _start_pos, _nt_id, result);          \
        return result;                                        \
    }

#define declare_recursive(base_name, num_branches)          \
    build_branch_protos(base_name, num_branches);           \
                                                            \
    tnode_t *                                               \
    base_name(parser_t *ctx)                                \
    {                                                       \
        nt_fn branches[num_branches + 1]                    \
            = build_branch_array(base_name, num_branches);  \
        return recursion_loop(ctx, num_branches, branches); \
    }
