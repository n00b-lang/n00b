// Parser support functions and data
//
// This file contains the non-declarative infrastructure for the parser:
// - Global data (keyword lists, sentinel nodes)
// - Terminal matching functions
// - State management functions
// - Left-recursion support
// - Parse node arena allocator

#include "parse_internal.h"
#include "base_alloc_shim.h"

// Global grammar selection flag (defined in branch_symbols.h)
bool ncc_use_reference_grammar = false;

// =============================================================================
// PARSE NODE ARENA ALLOCATOR
// =============================================================================
//
// The parser tries branches speculatively and backtracks on failure. Without
// memory management, failed branches leak nodes. This arena provides:
//
// - Fast bump-pointer allocation within chunks
// - mark/reset for efficient deallocation on backtrack
// - Zero-initialization of nodes (like calloc)

_Thread_local parse_arena_chunk_t *parse_arena_head    = nullptr;
_Thread_local parse_arena_chunk_t *parse_arena_current = nullptr;

static parse_arena_chunk_t *
arena_alloc_chunk(void)
{
    parse_arena_chunk_t *chunk = base_alloc(sizeof(parse_arena_chunk_t));
    if (chunk) {
        chunk->next = nullptr;
        chunk->used = 0;
    }
    return chunk;
}

void
parse_arena_init(void)
{
    // Initialize grammar selection on first call
    static bool branch_init_done = false;
    if (!branch_init_done) {
        branch_symbols_init();
        branch_init_done = true;
    }

    if (!parse_arena_head) {
        parse_arena_head    = arena_alloc_chunk();
        parse_arena_current = parse_arena_head;
    }
}

void
parse_arena_destroy(void)
{
    parse_arena_chunk_t *chunk = parse_arena_head;
    while (chunk) {
        parse_arena_chunk_t *next = chunk->next;
        base_dealloc(chunk);
        chunk = next;
    }
    parse_arena_head    = nullptr;
    parse_arena_current = nullptr;
}

void
parse_arena_reset(parse_arena_mark_t mark)
{
    if (!mark.chunk) {
        // Reset to empty - free all chunks except head, reset head
        parse_arena_chunk_t *chunk = parse_arena_head ? parse_arena_head->next : nullptr;
        while (chunk) {
            parse_arena_chunk_t *next = chunk->next;
            base_dealloc(chunk);
            chunk = next;
        }
        if (parse_arena_head) {
            parse_arena_head->next = nullptr;
            parse_arena_head->used = 0;
        }
        parse_arena_current = parse_arena_head;
        return;
    }

    // Free all chunks after the marked chunk
    parse_arena_chunk_t *chunk = mark.chunk->next;
    while (chunk) {
        parse_arena_chunk_t *next = chunk->next;
        base_dealloc(chunk);
        chunk = next;
    }
    mark.chunk->next = nullptr;

    // Reset position within marked chunk
    parse_arena_current       = mark.chunk;
    parse_arena_current->used = mark.position;
}

tnode_t *
alloc_tnode(void)
{
    // Ensure arena is initialized
    if (!parse_arena_current) {
        parse_arena_init();
    }

    // Need a new chunk?
    if (parse_arena_current->used >= PARSE_ARENA_CHUNK_SIZE) {
        parse_arena_chunk_t *new_chunk = arena_alloc_chunk();
        if (!new_chunk) {
            return nullptr;
        }
        parse_arena_current->next = new_chunk;
        parse_arena_current       = new_chunk;
    }

    // Allocate from current chunk
    tnode_t *node = &parse_arena_current->nodes[parse_arena_current->used++];

    // Zero-initialize (like calloc)
    *node = (tnode_t){0};

    return node;
}

// =============================================================================
// MEMO TABLE FOR PACKRAT PARSING
// =============================================================================
//
// Packrat parsing caches the result of each (position, nonterminal) pair.
// Trees are copied to the heap for caching and back to the arena on retrieval.

// Iterative tree copy to heap (malloc).
static tnode_t *
copy_tree_to_heap(tnode_t *root)
{
    if (!root) return nullptr;

    typedef struct { tnode_t *src; tnode_t *dst; int kid; } frame_t;

    int      cap   = 64;
    frame_t *stack = base_alloc(cap * sizeof(frame_t));
    int      sp    = 0;

    // Allocate root copy.
    tnode_t *root_copy = base_calloc(1, sizeof(tnode_t));
    *root_copy = (tnode_t){
        .nt = root->nt, .nt_id = root->nt_id, .tptr = root->tptr,
        .branch = root->branch, .num_kids = root->num_kids,
        .num_toks = root->num_toks, .id = root->id,
    };
    if (root->num_kids > 0 && root->kids) {
        root_copy->kids = list_alloc(root->num_kids);
    }

    stack[sp++] = (frame_t){.src = root, .dst = root_copy, .kid = 0};

    while (sp > 0) {
        frame_t *f = &stack[sp - 1];
        if (f->kid >= f->src->num_kids) {
            sp--;
            continue;
        }
        tnode_t *child = tnode_get_kid(f->src, f->kid);
        int ci = f->kid++;

        if (!child) {
            f->dst->kids->items[ci] = nullptr;
            continue;
        }

        if (IS_ELIDED(child)) {
            f->dst->kids->items[ci] = (tnode_t *)&elided_node;
            continue;
        }

        tnode_t *cc = base_calloc(1, sizeof(tnode_t));
        *cc = (tnode_t){
            .nt = child->nt, .nt_id = child->nt_id, .tptr = child->tptr,
            .branch = child->branch, .num_kids = child->num_kids,
            .num_toks = child->num_toks, .id = child->id,
            .parent = f->dst,
        };
        if (child->num_kids > 0 && child->kids) {
            cc->kids = list_alloc(child->num_kids);
        }
        f->dst->kids->items[ci] = cc;

        if (child->num_kids > 0) {
            if (sp >= cap) {
                cap  *= 2;
                stack = base_realloc(stack, cap * sizeof(frame_t));
            }
            stack[sp++] = (frame_t){.src = child, .dst = cc, .kid = 0};
        }
    }

    base_dealloc(stack);
    return root_copy;
}

// Iterative tree copy from heap to arena.
static tnode_t *
copy_tree_to_arena(tnode_t *root)
{
    if (!root) return nullptr;

    typedef struct { tnode_t *src; tnode_t *dst; int kid; } frame_t;

    int      cap   = 64;
    frame_t *stack = base_alloc(cap * sizeof(frame_t));
    int      sp    = 0;

    tnode_t *root_copy = alloc_tnode();
    *root_copy = (tnode_t){
        .nt = root->nt, .nt_id = root->nt_id, .tptr = root->tptr,
        .branch = root->branch, .num_kids = root->num_kids,
        .num_toks = root->num_toks, .id = root->id,
    };
    if (root->num_kids > 0 && root->kids) {
        root_copy->kids = list_alloc(root->num_kids);
    }

    stack[sp++] = (frame_t){.src = root, .dst = root_copy, .kid = 0};

    while (sp > 0) {
        frame_t *f = &stack[sp - 1];
        if (f->kid >= f->src->num_kids) {
            sp--;
            continue;
        }
        tnode_t *child = tnode_get_kid(f->src, f->kid);
        int ci = f->kid++;

        if (!child) {
            f->dst->kids->items[ci] = nullptr;
            continue;
        }

        if (IS_ELIDED(child)) {
            f->dst->kids->items[ci] = (tnode_t *)&elided_node;
            continue;
        }

        tnode_t *cc = alloc_tnode();
        *cc = (tnode_t){
            .nt = child->nt, .nt_id = child->nt_id, .tptr = child->tptr,
            .branch = child->branch, .num_kids = child->num_kids,
            .num_toks = child->num_toks, .id = child->id,
            .parent = f->dst,
        };
        if (child->num_kids > 0 && child->kids) {
            cc->kids = list_alloc(child->num_kids);
        }
        f->dst->kids->items[ci] = cc;

        if (child->num_kids > 0) {
            if (sp >= cap) {
                cap  *= 2;
                stack = base_realloc(stack, cap * sizeof(frame_t));
            }
            stack[sp++] = (frame_t){.src = child, .dst = cc, .kid = 0};
        }
    }

    base_dealloc(stack);
    return root_copy;
}

// Iterative free of heap-allocated tree.
static void
free_heap_tree(tnode_t *root)
{
    if (!root) return;

    int       cap   = 64;
    tnode_t **stack  = base_alloc(cap * sizeof(tnode_t *));
    int       sp     = 0;

    stack[sp++] = root;
    while (sp > 0) {
        tnode_t *n = stack[--sp];
        if (!n) continue;

        if (IS_ELIDED(n)) continue;

        if (n->kids) {
            for (int i = 0; i < n->num_kids; i++) {
                if (sp >= cap) {
                    cap  *= 2;
                    stack = base_realloc(stack, cap * sizeof(tnode_t *));
                }
                stack[sp++] = tnode_get_kid(n, i);
            }
            base_dealloc(n->kids);
        }
        base_dealloc(n);
    }
    base_dealloc(stack);
}

void
memo_init(parser_t *ctx)
{
    int num_positions = ctx->num_tokens + 1;
    ctx->memo = base_calloc(num_positions, sizeof(memo_entry_t *));
    if (!ctx->memo) {
        ctx->memo_size = 0;
        return;
    }
    ctx->memo_size = num_positions;

    for (int i = 0; i < num_positions; i++) {
        ctx->memo[i] = base_calloc(NT_COUNT, sizeof(memo_entry_t));
    }
}

void
memo_free(parser_t *ctx)
{
    if (!ctx->memo) return;

    for (int i = 0; i < ctx->memo_size; i++) {
        if (ctx->memo[i]) {
            for (int j = 0; j < NT_COUNT; j++) {
                memo_entry_t *entry = &ctx->memo[i][j];
                if (entry->result && entry->end_pos != MEMO_FAIL) {
                    free_heap_tree(entry->result);
                }
            }
            base_dealloc(ctx->memo[i]);
        }
    }
    base_dealloc(ctx->memo);
    ctx->memo      = nullptr;
    ctx->memo_size = 0;
}

bool
memo_check(parser_t *ctx, nt_type_t nt_id, tnode_t **result)
{
    if (!ctx->memo || ctx->pos >= ctx->memo_size || !ctx->memo[ctx->pos]) {
        return false;
    }

    memo_entry_t *entry = &ctx->memo[ctx->pos][nt_id];

    if (entry->end_pos == MEMO_EMPTY && entry->result == nullptr) {
        return false;
    }

    if (entry->end_pos == MEMO_FAIL) {
        *result = nullptr;
        return true;
    }

    *result  = copy_tree_to_arena(entry->result);
    ctx->pos = entry->end_pos;
    return true;
}

void
memo_store(parser_t *ctx, int start_pos, nt_type_t nt_id, tnode_t *result)
{
    if (!ctx->memo || start_pos >= ctx->memo_size || !ctx->memo[start_pos]) {
        return;
    }

    memo_entry_t *entry = &ctx->memo[start_pos][nt_id];

    // Free any previously cached tree.
    if (entry->result && entry->end_pos != MEMO_FAIL) {
        free_heap_tree(entry->result);
    }

    if (result) {
        entry->result  = copy_tree_to_heap(result);
        entry->end_pos = ctx->pos;
    }
    else {
        entry->result  = nullptr;
        entry->end_pos = MEMO_FAIL;
    }
}

// clang-format off

str_list(kw_type_alignment, "alignas", "_Alignas");
str_list(kw_type_specifier, "void", "char", "short", "int", "long", "float",
    "double", "signed", "unsigned", "bool", "_Bool", "_Complex",
    "_Decimal32", "_Decimal64", "_Decimal128", "_Float16", "_Float32", "_Float32x",
    "_Float64", "_Float64x", "_Float128", "__signed__", "__signed",
    "__builtin_va_list", "__int128", "__int128_t", "__uint128_t",
    // ARM/Clang built-in floating-point types
    "__fp16",   // 16-bit half-precision float (IEEE 754 binary16)
    "__bf16",   // bfloat16 (brain float, ML-optimized 16-bit float)
    "__mfp8",   // 8-bit minifloat (ARM AArch64, for ML inference)
    // ARM NEON/SVE vector types
    "__Float32x4_t", "__Float64x2_t",
    "__SVFloat32_t", "__SVFloat64_t", "__SVBool_t");
str_list(kw_fn_specifier, "inline", "_Noreturn", "__inline__", "__inline");
str_list(kw_once_id, "once");
str_list(kw_static_assert, "static_assert", "_Static_assert");
str_list(kw_storage_class, "auto", "constexpr", "extern", "register",
    "static", "thread_local", "typedef", "_Thread_local", "__thread");
str_list(kw_typeof, "typeof", "__typeof__", "__typeof");
str_list(kw_typeof_unqual, "typeof_unqual");
str_list(kw_typeid, "typeid", "__typeid__", "__typeid");
str_list(kw_typestr, "typestr", "__typestr__", "__typestr");
str_list(kw_type_qualifier, "const", "restrict", "volatile", "_Atomic",
    "__const__", "__const", "__restrict__", "__restrict", "__volatile__", "__volatile",
    "_Nullable", "_Nonnull", "_Null_unspecified", "__nullable", "__nonnull", "__null_unspecified");
// GCC extensions
str_list(kw_gcc_attribute, "__attribute__", "__attribute");
str_list(kw_gcc_extension, "__extension__");
str_list(kw_gcc_asm, "__asm__", "__asm", "asm");
str_list(kw_struct_or_union, "struct", "union");
str_list(kw_bitint, "_BitInt");
str_list(kw_static, "static");
str_list(kw_atomic, "_Atomic");
str_list(kw_enum, "enum");

str_list(kw_sizeof, "sizeof");
str_list(kw_alignof, "alignof", "_Alignof");
str_list(kw_countof, "_Countof");
str_list(kw_generic, "_Generic");
str_list(kw_default, "default");
str_list(kw_true_false_nullptr, "true", "false", "nullptr");
str_list(kw_builtin_va_arg, "__builtin_va_arg");
str_list(kw_builtin_types_compatible_p, "__builtin_types_compatible_p");
str_list(kw_c_va, "c_va");
str_list(kw_opaque, "opaque");
str_list(kw_package, "package");

str_list(kw_if, "if");
str_list(kw_else, "else");
str_list(kw_switch, "switch");
str_list(kw_case, "case");
str_list(kw_while, "while");
str_list(kw_do, "do");
str_list(kw_for, "for");
str_list(kw_goto, "goto");
str_list(kw_continue, "continue");
str_list(kw_break, "break");
str_list(kw_return, "return");
str_list(kw_keywords, "_kargs");

str_list(op_unary, "&", "*", "+", "-", "~", "!");
str_list(op_assign, "=", "*=", "/=", "%=", "+=", "-=", "<<=", ">>=", "&=", "^=", "|=");

// clang-format on

const tnode_t elided_node = {
    .nt       = "<<elided>>",
    .tptr     = nullptr,
    .id       = 0,
    .num_kids = 0,
    .num_toks = 0,
    .kids     = {},
};

const tok_t eof_token = {
    .offset      = -1,
    .replacement = nullptr,
    .len         = -1,
    .line_no     = -1,
    .type        = TT_ERR,
};

static int next_node_id = 1;

/**
 * @brief Allocate a parse tree node.
 *
 * Creates a node with branch=0. The actual branch number is set by the
 * wrapper functions (declare_nt/recursion_loop) after the branch function
 * returns successfully.
 */
[[gnu::noinline]] tnode_t *
node_alloc(parser_t *ctx, char *nt)
{
    tnode_t *node = alloc_tnode();

    assert(node);

    scope_t *scope = nullptr;
    if (ctx && ctx->symtab) {
        scope = ctx->symtab->current_scope;
    }

    *node = (tnode_t){
        .nt       = nt,
        .nt_id    = nt_lookup(nt),
        .branch   = 0, // Set by wrapper after successful match
        .id       = next_node_id++,
        .tptr     = nullptr,
        .scope    = scope,
        .num_kids = 0,
        .num_toks = 0,
    };

    return node;
}

tok_t *
get_tok(parser_t *ctx)
{
    if (ctx->pos >= ctx->num_tokens) {
        return (tok_t *)&eof_token;
    }
    else {
        return (tok_t *)&ctx->tokens[ctx->pos];
    }
}

void
parse_advance(parser_t *ctx)
{
    while (ctx->pos < ctx->num_tokens) {
        const tok_t *tok = &ctx->tokens[++ctx->pos];

        switch (tok->type) {
        case TT_COMMENT:
        case TT_WS:
        case TT_PREPROC:
            continue;
        default:
            return;
        }
    }
}

void
parse_prime(parser_t *ctx)
{
    while (ctx->pos < ctx->num_tokens) {
        const tok_t *tok = &ctx->tokens[ctx->pos];

        switch (tok->type) {
        case TT_COMMENT:
        case TT_WS:
        case TT_PREPROC:
            ctx->pos++;
            continue;
        default:
            return;
        }
    }
}

void
add_kid(tnode_t *parent, tnode_t *kid)
{
    int old_count = parent->num_kids;
    int new_count = old_count + 1;

    // Grow the kids list
    list_t *new_kids = list_alloc(new_count);
    assert(new_kids != nullptr);

    // Copy existing children
    if (parent->kids) {
        for (int i = 0; i < old_count; i++) {
            new_kids->items[i] = parent->kids->items[i];
        }
        base_dealloc(parent->kids);
    }

    // Add new child
    new_kids->items[old_count] = kid;
    parent->kids               = new_kids;
    parent->num_kids           = new_count;

    assert(!kid->parent);

    if (kid != (tnode_t *)&elided_node) {
        kid->parent = parent;
    }
}

void
add_placeholder(tnode_t *node)
{
    add_kid(node, (tnode_t *)&elided_node);
}

void
pstate_save(parser_t *ctx, save_state_t *dst)
{
    *dst = (save_state_t){
        .node           = cur_node(ctx),
        .recursive      = ctx->recursive_node,
        .token_position = ctx->pos,
        .arena_mark     = parse_arena_mark(),
    };

    pdebug("Saving in: %s (cur tok = %s)\n",
           cur_node(ctx) ? cur_node(ctx)->nt : "<<root>>",
           extract((ncc_buf_t *)ctx->input, get_tok(ctx)));
}

void
pstate_restore(parser_t *ctx, save_state_t *src)
{
    // Reset arena to recover memory from failed branch
    parse_arena_reset(src->arena_mark);

    ctx->cur_node       = src->node;
    ctx->recursive_node = src->recursive;
    ctx->pos            = src->token_position;
    parse_prime(ctx);
    pdebug("Restoring in: %s (cur tok = %s)\n",
           cur_node(ctx)->nt,
           extract((ncc_buf_t *)ctx->input, get_tok(ctx)));

    if (ctx->recursive_node) {
        ctx->recursive_node->parent = nullptr;
    }
}

tnode_t *
evaluate_nonterminal(parser_t *ctx, nt_fn nonterminal)
{
    save_state_t save_data;

    pstate_save(ctx, &save_data);

    tnode_t *result = (*nonterminal)(ctx);

    ctx->cur_node = save_data.node;

    if (!result) {
        pstate_restore(ctx, &save_data);
    }
    else {
        ctx->cur_node = save_data.node;
        pdebug("No restore; cur tok = %s\n",
               extract((ncc_buf_t *)ctx->input, get_tok(ctx)));
    }

    return result;
}

bool
_optional_nt(parser_t *ctx, nt_fn nonterminal_top)
{
    tnode_t *stash   = cur_node(ctx);
    tnode_t *subtree = evaluate_nonterminal(ctx, nonterminal_top);
    if (subtree) {
        add_kid(stash, subtree);
    }
    else {
        add_placeholder(stash);
    }

    ctx->cur_node = stash;

    return subtree != nullptr;
}

bool
_required_nt(parser_t *ctx, nt_fn nonterminal_top)
{
    tnode_t *subtree = evaluate_nonterminal(ctx, nonterminal_top);

    if (subtree) {
        if (!subtree->parent) {
            add_kid(cur_node(ctx), subtree);
        }
        return true;
    }

    return false;
}

char *
check_strlist_tok(strlist_t *list, const char *text, int len)
{
    for (int i = 0; i < list->num_items; i++) {
        char  *item     = list->items[i];
        size_t item_len = strlen(item);
        if ((size_t)len == item_len && !memcmp(text, item, item_len)) {
            return item;
        }
    }
    return nullptr;
}

// Match a token of given type against a string list
static tnode_t *
optional_token_in_list(parser_t *ctx, strlist_t *list, ttype_t expected_type)
{
    tok_t *tok = get_tok(ctx);

    if (tok->type != expected_type) {
        add_placeholder(cur_node(ctx));
        return nullptr;
    }

    int         tok_len;
    const char *tok_text = tok_text_ptr(ctx->input, tok, &tok_len);
    char       *match    = check_strlist_tok(list, tok_text, tok_len);
    if (match) {
        tnode_t *n = node_alloc(ctx, match);
        n->tptr    = get_tok(ctx);
        parse_advance(ctx);
        add_kid(cur_node(ctx), n);
        return n;
    }

    add_placeholder(cur_node(ctx));
    return nullptr;
}

tnode_t *
_optional_keyword(parser_t *ctx, strlist_t *list)
{
    return optional_token_in_list(ctx, list, TT_KEYWORD);
}

tnode_t *
_optional_named_identifier(parser_t *ctx, strlist_t *list)
{
    return optional_token_in_list(ctx, list, TT_ID);
}

tnode_t *
_optional_op(parser_t *ctx, char *op)
{
    tok_t *tok    = get_tok(ctx);
    size_t op_len = strlen(op);

    int         tok_len;
    const char *tok_text = tok_text_ptr(ctx->input, tok, &tok_len);

    if (tok->type == TT_PUNCT && (size_t)tok_len == op_len && !memcmp(tok_text, op, op_len)) {
        tnode_t *n = node_alloc(ctx, op);
        n->tptr    = get_tok(ctx);
        parse_advance(ctx);
        add_kid(cur_node(ctx), n);
        return n;
    }

    add_placeholder(cur_node(ctx));
    return nullptr;
}

tnode_t *
_optional_op_list(parser_t *ctx, strlist_t *list)
{
    tok_t *tok = get_tok(ctx);

    if (tok->type != TT_PUNCT) {
        add_placeholder(cur_node(ctx));
        return nullptr;
    }

    int         tok_len;
    const char *tok_text = tok_text_ptr(ctx->input, tok, &tok_len);
    char       *match    = check_strlist_tok(list, tok_text, tok_len);
    if (match) {
        tnode_t *n = node_alloc(ctx, match);
        n->tptr    = get_tok(ctx);
        parse_advance(ctx);
        add_kid(cur_node(ctx), n);
        return n;
    }

    add_placeholder(cur_node(ctx));
    return nullptr;
}

tnode_t *
provided_identifier(parser_t *ctx)
{
    tok_t *tok = get_tok(ctx);

    if (tok->type != TT_ID) {
        return nullptr;
    }

    tnode_t *n = named_node(ctx);
    n->tptr    = tok;
    parse_advance(ctx);
    return n;
}

tnode_t *
constant(parser_t *ctx)
{
    tok_t *tok = get_tok(ctx);

    switch (tok->type) {
    case TT_NUM:
    case TT_CHR: {
        tnode_t *n = node_alloc(ctx, "constant");
        n->tptr    = tok;
        parse_advance(ctx);
        return n;
    }
    case TT_KEYWORD: {
        int         tok_len;
        const char *tok_text = tok_text_ptr(ctx->input, tok, &tok_len);
        if (check_strlist_tok(kw_true_false_nullptr, tok_text, tok_len)) {
            tnode_t *n = node_alloc(ctx, "constant");
            n->tptr    = tok;
            parse_advance(ctx);
            return n;
        }
        return nullptr;
    }
    default:
        return nullptr;
    }
}

tnode_t *
string_literal(parser_t *ctx)
{
    tok_t *tok = get_tok(ctx);

    if (tok->type != TT_STR) {
        return nullptr;
    }

    tnode_t *n  = node_alloc(ctx, "string_literal");
    n->tptr     = tok;
    n->num_toks = 1;
    parse_advance(ctx);

    while (get_tok(ctx)->type == TT_STR) {
        n->num_toks++;
        parse_advance(ctx);
    }

    return n;
}

tnode_t *
typedef_name_terminal(parser_t *ctx)
{
    tok_t *tok = get_tok(ctx);

    if (tok->type != TT_ID) {
        return nullptr;
    }

    char *name = extract((ncc_buf_t *)ctx->input, tok);

    if (!st_is_typedef(ctx->symtab, name)) {
        return nullptr;
    }

    tnode_t *n = node_alloc(ctx, "typedef_name");
    n->tptr    = tok;
    parse_advance(ctx);
    return n;
}

tnode_t *
non_bracket_token(parser_t *ctx)
{
    tok_t *tok = get_tok(ctx);

    if (tok->type == TT_ERR) {
        return nullptr;
    }

    int         tok_len;
    const char *tok_text = tok_text_ptr(ctx->input, tok, &tok_len);
    char        c        = tok_text[0];
    if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}') {
        return nullptr;
    }

    tnode_t *n = node_alloc(ctx, "token");
    n->tptr    = tok;
    parse_advance(ctx);
    return n;
}

bool
can_be_left_recursion(tnode_t *node)
{
    for (int i = 0; i < node->num_kids; i++) {
        if (tnode_get_kid(node, i) != (tnode_t *)&elided_node) {
            return false;
        }
    }

    return true;
}

bool
_direct_self_call(parser_t *ctx)
{
    if (!can_be_left_recursion(cur_node(ctx))) {
        return false;
    }

    if (!ctx->recursive_node) {
        return false;
    }

    assert(!ctx->recursive_node->parent);
    add_kid(cur_node(ctx), ctx->recursive_node);
    ctx->recursive_node = nullptr;
    return true;
}

bool
_indirect_self_call(parser_t *ctx)
{
    if (!can_be_left_recursion(cur_node(ctx))) {
        return false;
    }

    if (!ctx->recursive_node) {
        return false;
    }

    assert(!ctx->recursive_node->parent);
    add_kid(cur_node(ctx), ctx->recursive_node);
    ctx->recursive_node = cur_node(ctx);
    return true;
}

void
_indirect_self_opt(parser_t *ctx)
{
    if (!_indirect_self_call(ctx)) {
        add_placeholder(cur_node(ctx));
    }
}

[[gnu::noinline]] tnode_t *
recursion_loop(parser_t *ctx, int n, nt_fn *branches)
{
    tnode_t     *item   = nullptr;
    tnode_t     *result = nullptr;
    save_state_t start;
    save_state_t state;

    ctx->recursive_node = nullptr;

    pstate_save(ctx, &start);

    do {
        result              = item;
        ctx->recursive_node = item;
        pstate_save(ctx, &state);

        for (int i = 0; i < n; i++) {
            // Save our recursive_node before the branch call - inner recursion_loops
            // may modify it, so we need to check against this specific value
            tnode_t *our_recursive = ctx->recursive_node;
            item                   = (*(branches[i]))(ctx);

            if (!item || (result && state.token_position == ctx->pos)) {
                pstate_restore(ctx, &state);
                continue;
            }

            // Set branch number on successful match
            item->branch = (uint8_t)i;

            // If OUR recursive_node was not consumed (unchanged), the branch
            // succeeded without using left recursion - stop and keep result
            if (our_recursive && ctx->recursive_node == our_recursive) {
                return result;
            }

            break;
        }
    } while (item);

    return result;
}

void
enter_function_scope(parser_t *ctx)
{
    ctx->saved_lt = ctx->label_table;
    lt_init(&ctx->func_lt);
    ctx->label_table = &ctx->func_lt;
}

void
exit_function_scope(parser_t *ctx)
{
    ctx->label_table = ctx->saved_lt;
}

void
enter_block_scope(parser_t *ctx)
{
    if (ctx->symtab) {
        st_push_scope(ctx->symtab, ctx->cur_node);
    }
}

void
exit_block_scope(parser_t *ctx)
{
    if (ctx->symtab) {
        st_pop_scope(ctx->symtab);
    }
}

#if defined(PDEBUG)
void
pdebug_push(parser_t *ctx, char *nt)
{
    pdebug_t *rec = base_calloc(1, sizeof(pdebug_t));
    *rec          = (pdebug_t){
        .next = nullptr,
        .prev = ctx->debug_cur,
        .nt   = nt,
    };
    if (ctx->debug_cur) {
        ctx->debug_cur->next = rec;
    }
    ctx->debug_cur = rec;
}

void
pdebug_pop(parser_t *ctx)
{
    if (ctx->debug_cur) {
        ctx->debug_cur = ctx->debug_cur->prev;
    }
}

void
pdebug_show(parser_t *ctx)
{
    pdebug("Parse stack: ");
    pdebug_t *cur = ctx->debug_root;
    while (cur) {
        pdebug("%s -> ", cur->nt);
        cur = cur->next;
    }
    pdebug("(end)\n");
}
#endif

// =============================================================================
// PER-NT ERROR HANDLERS
// =============================================================================
//
// When all branches of a nonterminal fail, the parser silently backtracks.
// For NTs that sit below an already-committed parse point (e.g. typeid_atom
// inside a typeid(...) call where "typeid" and "(" already matched), we want
// a fatal diagnostic instead of letting the failure ripple up to a confusing
// "Parser stopped at token 'struct'" far from the real problem.
//
// Handlers are registered once at parser startup and indexed by NT ID.
// The nt_check_error() inline in parse_internal.h does nothing when the
// table entry is nullptr (zero overhead for NTs without handlers).

nt_error_fn nt_error_handlers[NT_COUNT] = {0};

void
register_nt_error_handler(nt_type_t nt_id, nt_error_fn handler)
{
    if (nt_id > NT_NONE && nt_id < NT_COUNT) {
        nt_error_handlers[nt_id] = handler;
    }
}

// ---------------------------------------------------------------------------
// Handler: typeid_atom
//
// Fires when typeid(X) is entered (typeid + '(' already matched by
// synthetic_identifier) but the argument X doesn't parse as either a
// string literal or a typeof_specifier_argument.  The most common cause
// is an unknown typedef name (e.g. uint64_t without #include <stdint.h>).
// ---------------------------------------------------------------------------
static void
err_typeid_atom(parser_t *ctx, int start_pos)
{
    tok_t      *tok = get_tok(ctx);
    const char *file;

    // Walk backward from start_pos to find the most recent src_file
    file = nullptr;
    for (int i = start_pos; i >= 0; i--) {
        if (ctx->tokens[i].src_file) {
            file = ctx->tokens[i].src_file;
            break;
        }
    }
    if (!file) {
        file = ctx->lex->in_file;
    }

    int         tok_len;
    const char *tok_text = tok_text_ptr(ctx->input, tok, &tok_len);

    ncc_error(
        "%s:%d: '%.*s' is not a recognized type name (in typeid).\n"
        "       If this is a typedef from a system header, "
        "ensure the header is #included.\n",
        file,
        tok->line_no,
        tok_len > 60 ? 60 : tok_len,
        tok_text);
    exit(1);
}

void
nt_error_init(void)
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;

    register_nt_error_handler(NT_typeid_atom, err_typeid_atom);
}
