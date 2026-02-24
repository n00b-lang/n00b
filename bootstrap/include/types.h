/**
 * @file types.h
 * @brief Type parsing, normalization, and AST node definitions.
 *
 * This header defines the parse tree node structure (tnode_t) used by the
 * parser to represent C source code as a concrete syntax tree.
 */
#pragma once

#include <stdlib.h>
#include "base_alloc_shim.h"
#include "lex.h"
#include "nt_types.h"

/** @name Parse Tree Node (tnode_t)
 *
 * The parser produces a concrete syntax tree where each node represents either
 * a grammar non-terminal (production rule) or a terminal (token). The tree
 * preserves all syntactic information including operators, keywords, and
 * punctuation.
 *
 * ## Node Types
 *
 * 1. **Non-terminal nodes** (grammar rules):
 *    - `nt` = branch function name (e.g., "declaration_1", "expression_0")
 *    - `tptr` = nullptr
 *    - `kids` = child nodes from the production
 *
 * 2. **Terminal nodes** (tokens):
 *    - `nt` = token text (e.g., "int", "+", "return", identifier text)
 *    - `tptr` = pointer to the source token
 *    - `kids` = empty (num_kids == 0)
 *
 * 3. **Elided nodes** (optional items not present):
 *    - `nt` = "<<elided>>"
 *    - Used as placeholder when optional grammar elements are absent
 *    - Maintains consistent child positions across parse trees
 *
 * ## Branch Naming Convention
 *
 * Non-terminal nodes are named after their grammar production with a numeric
 * suffix indicating which alternative matched. For example, if the grammar is:
 *
 *   primary-expression:
 *     identifier           // branch 0
 *     constant             // branch 1
 *     string-literal       // branch 2
 *     ( expression )       // branch 3
 *
 * Then parsing `42` produces a node with `nt = "primary_expression_1"`.
 *
 * This allows tree walkers to determine exactly which grammar alternative
 * was matched without re-parsing or complex pattern matching.
 *
 * ## Tree Structure Example
 *
 * For input `x + 1`, the tree looks like:
 *
 *   additive_expression_1
 *   +-- additive_expression_0
 *   |   +-- ... (chain to primary_expression_0)
 *   |       +-- identifier [tptr -> "x"]
 *   +-- "+" [tptr -> "+"]
 *   +-- multiplicative_expression_0
 *       +-- ... (chain to primary_expression_1)
 *           +-- constant [tptr -> "1"]
 *
 * ## Memory Management
 *
 * - Nodes are allocated with base_calloc() and must be freed by the caller
 * - The `nt` field points to static strings (branch names) or token text
 * - The `tptr` field points into the token array (do not free separately)
 * - Child nodes are owned by their parent and should be freed recursively
 *
 * ## Thread Safety
 *
 * Parse trees are not thread-safe. Each tree should be accessed by a single
 * thread, or external synchronization must be provided.
 * @{
 */

/**
 * @brief Maximum number of children per node (grammar elements per production).
 *
 * This value bounds the number of elements in any single grammar production.
 * The C grammar's longest productions typically have fewer than 10 elements
 * (e.g., for-statement has ~7, function definitions have ~5). The value 32
 * provides ample headroom for:
 *
 *   1. Complex productions with many optional elements
 *   2. Future grammar extensions (C2Y and beyond)
 *   3. Temporary constructs during tree transformations
 *
 * The power-of-2 size (32 * 8 = 256 bytes for kids[]) provides good cache
 * alignment. Increasing this value increases memory per node; decreasing it
 * risks overflow on complex grammar constructs. If you encounter an assertion
 * failure on num_kids, increase this value.
 *
 * @note This constant is also used by type_normalize.c for norm_node_t bounds
 * checking. Any change here affects both parse trees and normalized type trees.
 */
#define MAX_TYPE_ELEMENTS 32

/**
 * @brief Generic list structure for variable-length arrays.
 */
typedef struct {
    int   nitems;  /**< Number of items in the list */
    void *items[]; /**< Flexible array of item pointers */
} ncc_list_t;

/**
 * @brief Allocate a list with space for elements.
 * @param nitems Number of elements to allocate space for
 * @return Newly allocated list, or nullptr on allocation failure
 */
static inline ncc_list_t*
ncc_list_alloc(int nitems)
{
    ncc_list_t*result = base_calloc(1, sizeof(ncc_list_t) + sizeof(void *) * nitems);
    result->nitems = nitems;

    return result;
}

/**
 * @brief Get the number of items in a list.
 */
static inline int
ncc_list_len(ncc_list_t*list)
{
    return list ? list->nitems : 0;
}

/**
 * @brief Get an item from a list by index.
 */
static inline void *
ncc_list_get(ncc_list_t*list, int index)
{
    if (!list || index < 0 || index >= list->nitems) {
        return nullptr;
    }
    return list->items[index];
}

/**
 * @brief Append an item to a list, reallocating as needed.
 * @param old_list Existing list (may be nullptr)
 * @param item Item to append
 * @return New list containing all old items plus the new item
 * @note The old list is freed. Caller must use returned pointer.
 */
static inline ncc_list_t*
ncc_list_append(ncc_list_t*old_list, void *item)
{
    int old_count = old_list ? old_list->nitems : 0;
    int new_count = old_count + 1;

    ncc_list_t*new_list = ncc_list_alloc(new_count);
    if (!new_list) {
        return old_list;
    }

    for (int i = 0; i < old_count; i++) {
        new_list->items[i] = old_list->items[i];
    }
    new_list->items[old_count] = item;

    base_dealloc(old_list);
    return new_list;
}

/** @} */

/** @name tnode_t - Parse Tree Node
 * @{
 */

typedef struct tnode_t          tnode_t;
typedef struct rewrite_origin_t rewrite_origin_t;
typedef struct scope_t          scope_t;

/**
 * @brief Origin tracking for synthetic/rewritten nodes.
 *
 * When a parse tree node is replaced by a rewrite operation, the new
 * synthetic node stores a reference to the original source location.
 * This enables:
 * - Accurate error messages pointing to original source
 * - #line directives in emitted code
 * - Debugging of rewrite transformations
 */
struct rewrite_origin_t {
    tnode_t *original_node;       /**< Original node that was replaced */
    int      original_start_line; /**< Start line of original source */
    int      original_end_line;   /**< End line of original source */
    char    *rewrite_name;        /**< Name of the rewrite (e.g., "foreach_expand") */
};

/**
 * @brief Parse tree node representing a grammar production or terminal token.
 *
 * @par Usage Example:
 * @code
 *   // Check if node is a terminal
 *   if (node->tptr != nullptr) {
 *       // Terminal node - access token via node->tptr
 *       char *text = extract(buf, node->tptr);
 *   }
 *
 *   // Iterate over children
 *   for (int i = 0; i < node->num_kids; i++) {
 *       tnode_t *child = node->kids[i];
 *       if (strcmp(child->nt, "<<elided>>") == 0) {
 *           // Optional element was not present
 *       }
 *   }
 *
 *   // Check which grammar branch matched
 *   if (strcmp(node->nt, "declaration_0") == 0) {
 *       // Matched: attr-spec-seq decl-specs init-decl-list ;
 *   } else if (strcmp(node->nt, "declaration_1") == 0) {
 *       // Matched: decl-specs init-decl-list? ;
 *   }
 * @endcode
 */
struct tnode_t {
    char             *nt;       /**< Node name (branch name or token text) */
    nt_type_t         nt_id;    /**< NT enum for fast comparisons (NT_NONE for terminals) */
    uint8_t           branch;   /**< Branch number (0 for "foo_0", 1 for "foo_1", etc.) */
    tok_t            *tptr;     /**< Source token (terminals only, nullptr for non-terminals) */
    tnode_t          *parent;   /**< Parent node (nullptr for root) */
    rewrite_origin_t *origin;   /**< Origin info for synthetic nodes (nullptr for original) */
    scope_t          *scope;    /**< Scope where this node was created (for symbol lookups) */
    ncc_list_t          *kids;     /**< Child nodes (dynamic list of tnode_t*) */
    int               id;       /**< Unique node ID for debugging */
    int               num_kids; /**< Number of children (mirrors kids->nitems) */
    int               num_toks; /**< For passthrough text (internal use) */
};

/**
 * @brief Get child node at index.
 * @param node Parent node
 * @param idx Child index
 * @return Child node at index, or nullptr if out of bounds
 */
static inline tnode_t *
tnode_get_kid(tnode_t *node, int idx)
{
    if (!node || !node->kids || idx < 0) {
        return nullptr;
    }
    // num_kids is intended to mirror kids->nitems, but some transform paths
    // can transiently desynchronize them. Guard on the backing list bounds.
    if (idx >= node->kids->nitems || idx >= node->num_kids) {
        return nullptr;
    }
    return (tnode_t *)node->kids->items[idx];
}

/**
 * @brief Set child node at index (does not update num_kids).
 * @param node Parent node
 * @param idx Child index
 * @param kid Child node to set
 */
static inline void
tnode_set_kid(tnode_t *node, int idx, tnode_t *kid)
{
    if (node && node->kids && idx >= 0 && idx < node->kids->nitems) {
        node->kids->items[idx] = kid;
    }
}

/** @brief Sentinel node for elided (optional but not present) parse tree elements. */
extern const tnode_t elided_node;

/** @brief Check if a node is the shared elided sentinel. */
#define IS_ELIDED(node) ((node) == (const tnode_t *)&elided_node)

/**
 * @brief Get the token from an identifier node.
 *
 * Handles both old-style terminal identifier nodes (where tptr is set
 * directly) and the new non-terminal wrapper (identifier -> provided_identifier
 * or synthetic_identifier).
 *
 * @param node An identifier node
 * @return The token pointer, or nullptr if not found
 */
static inline tok_t *
identifier_tok(tnode_t *node)
{
    if (!node) {
        return nullptr;
    }
    if (node->tptr) {
        return node->tptr;
    }
    // Walk children to find the terminal with a token
    for (int i = 0; i < node->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(node, i);
        if (kid && kid->tptr) {
            return kid->tptr;
        }
    }
    return nullptr;
}

/** @} */

/** @name Normalized Type Tree
 * @brief For type canonicalization.
 * @{
 */

/** @brief Forward declaration for normalized type tree node. */
typedef struct norm_node_t norm_node_t;

/** @} */

/** @name Utility Functions
 * @brief From utils.c
 * @{
 */

/**
 * @brief Allocate a token array for a buffer.
 * @param buf Buffer to allocate tokens for
 * @return Newly allocated token array
 */
extern tok_t *alloc_tokens(ncc_buf_t *buf);

/**
 * @brief Count newlines between buffer start and token position.
 * @param state Lexer state
 * @param tok Token to count newlines before
 * @return Number of newlines
 */
extern int count_newlines(lex_t *state, tok_t *tok);

/**
 * @brief Get actual parameters from a wrapper transformation.
 * @param ctx Transform context
 * @param start Start index
 * @param end End index
 * @return List of actual parameters
 */
extern ncc_list_t*get_wrapper_actuals(tok_xform_t *ctx, int start, int end);

/**
 * @brief Join list items with a separator string.
 * @param list List of strings to join
 * @param sep Separator string
 * @return Newly allocated joined string
 */
extern char *join(ncc_list_t*list, char *sep);

/** @} */

/** @name Type Normalization
 * @brief From type_normalize.c
 * @{
 */

/**
 * @brief Normalize a type string to canonical form.
 * @param str Type string to normalize
 * @return Normalized type string (e.g., "const int *" -> "int const *")
 */
extern char *normalize_type(char *str);

/**
 * @brief Generate a compact encoding from a normalized type tree.
 * @param node Normalized type tree root
 * @return Compact encoding string
 */
extern char *encoding_from_normalized_type_tree(norm_node_t *node);

/**
 * @brief Convert a normalized type tree back to a string.
 * @param t Normalized type tree root
 * @return String representation of the type
 */
extern char *normalized_type_tree_to_string(norm_node_t *t);

/**
 * @brief Parse a type string into a normalized type tree.
 * @param type_as_string Type string to parse
 * @return Normalized type tree
 */
extern norm_node_t *type_to_normalized_type_tree(char *type_as_string);

/**
 * @brief Normalize a parse tree node to a type tree.
 *
 * Transforms any tnode_t subtree into a normalized norm_node_t*.
 *
 * @param input Source buffer
 * @param node Parse tree node to normalize
 * @return Normalized type tree
 */
extern norm_node_t *normalize_tokens_to_type_tree(ncc_buf_t *input, tnode_t *node);

/**
 * @brief Generate a mangled identifier from a type string.
 * @param type_as_string Type string to mangle
 * @return Mangled identifier string
 */
extern char     *get_munged_identifier(char *type_as_string);
extern uint64_t  get_type_hash_u64(char *type_as_string);

// Find the leftmost leaf token in a tnode subtree.
static inline tok_t *
tnode_first_tok(tnode_t *node)
{
    if (!node) {
        return nullptr;
    }
    if (node->tptr) {
        return node->tptr;
    }
    for (int i = 0; i < node->num_kids; i++) {
        tok_t *t = tnode_first_tok(tnode_get_kid(node, i));
        if (t) {
            return t;
        }
    }
    return nullptr;
}

// Find the rightmost leaf token in a tnode subtree.
static inline tok_t *
tnode_last_tok(tnode_t *node)
{
    if (!node) {
        return nullptr;
    }
    if (node->tptr) {
        return node->tptr;
    }
    for (int i = node->num_kids - 1; i >= 0; i--) {
        tok_t *t = tnode_last_tok(tnode_get_kid(node, i));
        if (t) {
            return t;
        }
    }
    return nullptr;
}

// Collect leaf-token text from a tnode subtree into a buffer,
// separated by spaces.  Handles both source and synthetic tokens
// via tok_text_ptr().
static inline ncc_buf_t *
_collect_tnode_text(tnode_t *node, ncc_buf_t *input, ncc_buf_t *acc, bool *need_space)
{
    if (!node) {
        return acc;
    }
    if (node->tptr) {
        int         tlen;
        const char *txt = tok_text_ptr(input, node->tptr, &tlen);
        if (tlen > 0) {
            if (*need_space) {
                acc = ncc_buf_concat(acc, " ", 1);
            }
            acc         = ncc_buf_concat(acc, (char *)txt, tlen);
            *need_space = true;
        }
        return acc;
    }
    for (int i = 0; i < node->num_kids; i++) {
        acc = _collect_tnode_text(tnode_get_kid(node, i), input, acc, need_space);
    }
    return acc;
}

static inline char *
normalize_type_node(tnode_t *node, ncc_buf_t *input)
{
    // Extract the raw type string from the tnode's token span,
    // then delegate to get_munged_identifier which uses the
    // token-level normalization (same algorithm as main NCC).
    tok_t *first = tnode_first_tok(node);
    tok_t *last  = tnode_last_tok(node);
    if (!first || !last) {
        return nullptr;
    }

    // If any leaf token is synthetic (has replacement text), we can't
    // use the offset-based fast path because synthetic tokens don't
    // reference the source buffer.  Walk the subtree instead.
    if (first->replacement || last->replacement) {
        ncc_buf_t *acc        = ncc_buf_alloc(0);
        bool       need_space = false;
        acc                   = _collect_tnode_text(node, input, acc, &need_space);

        // Null-terminate for get_munged_identifier.
        char *type_str = base_alloc(acc->len + 1);
        memcpy(type_str, acc->data, acc->len);
        type_str[acc->len] = '\0';
        base_dealloc(acc);

        char *result = get_munged_identifier(type_str);
        base_dealloc(type_str);
        return result;
    }

    int start = first->offset;
    int end   = last->offset + last->len;
    int len   = end - start;

    char *type_str = base_alloc(len + 1);
    memcpy(type_str, input->data + start, len);
    type_str[len] = '\0';

    char *result = get_munged_identifier(type_str);
    base_dealloc(type_str);
    return result;
}

/** @} */

/** @name Error Reporting
 * @brief Simple error reporting macros.
 * @{
 */

/**
 * @brief Print an error message to stderr.
 * @param ... Format string and arguments (like printf)
 */
#define ncc_error(...) (void)fprintf(stderr, "error: " __VA_ARGS__)

/**
 * @brief Print a warning message to stderr.
 * @param ... Format string and arguments (like printf)
 */
#define ncc_warning(...) (void)fprintf(stderr, "warning: " __VA_ARGS__)

/** @} */

/** @name Parse Node Arena
 * @brief Arena allocator for parse tree nodes with mark/reset for backtracking.
 *
 * During parsing, branches are tried speculatively and many fail, requiring
 * backtracking. Without an arena, these failed attempts leak memory. The arena
 * provides:
 *
 * - Fast allocation (bump pointer within chunks)
 * - mark() to save current position before trying a branch
 * - reset() to free all allocations since the mark if branch fails
 *
 * Usage:
 *   parse_arena_mark_t mark = parse_arena_mark();
 *   // ... try parsing branch ...
 *   if (failed) {
 *       parse_arena_reset(mark);  // Free nodes allocated in failed branch
 *   }
 * @{
 */

/** Number of tnode_t per arena chunk (tuned for typical backtracking depth) */
#define PARSE_ARENA_CHUNK_SIZE 1024

/** Size of each kids arena chunk in bytes (256KB) */
#define KIDS_ARENA_CHUNK_SIZE (256 * 1024)

/** @brief Arena chunk containing parse nodes */
typedef struct parse_arena_chunk_t parse_arena_chunk_t;
struct parse_arena_chunk_t {
    parse_arena_chunk_t *next; /**< Next chunk in list */
    int                  used; /**< Number of nodes used in this chunk */
    tnode_t              nodes[PARSE_ARENA_CHUNK_SIZE]; /**< Node storage */
};

/** @brief Byte arena chunk for kids list allocations during parsing */
typedef struct kids_arena_chunk_t kids_arena_chunk_t;
struct kids_arena_chunk_t {
    kids_arena_chunk_t *next;     /**< Next chunk in list */
    int                 used;     /**< Bytes used in this chunk */
    int                 capacity; /**< Bytes available in data[] */
    char                data[];   /**< Bump-allocated storage */
};

/** @brief Arena state for mark/reset (kids arena is never reset) */
typedef struct {
    parse_arena_chunk_t *chunk;    /**< Node chunk at mark time */
    int                  position; /**< Position within node chunk at mark time */
} parse_arena_mark_t;

/** @brief Global parse node arena (thread-local for future MT support) */
extern _Thread_local parse_arena_chunk_t *parse_arena_head;
extern _Thread_local parse_arena_chunk_t *parse_arena_current;

/** @brief Global kids byte arena (thread-local for future MT support) */
extern _Thread_local kids_arena_chunk_t *kids_arena_head;
extern _Thread_local kids_arena_chunk_t *kids_arena_current;

/**
 * @brief Initialize the parse arena (call before parsing).
 */
void parse_arena_init(void);

/**
 * @brief Free all arena memory (call after parsing complete).
 */
void parse_arena_destroy(void);

/**
 * @brief Mark current arena position for potential rollback.
 * @return Mark that can be passed to parse_arena_reset()
 */
static inline parse_arena_mark_t
parse_arena_mark(void)
{
    return (parse_arena_mark_t){
        .chunk    = parse_arena_current,
        .position = parse_arena_current ? parse_arena_current->used : 0,
    };
}

/**
 * @brief Reset arena to a previous mark, freeing all allocations since.
 * @param mark Previously saved mark from parse_arena_mark()
 */
void parse_arena_reset(parse_arena_mark_t mark);

/** @} */

/** @name Allocation Helpers
 * @brief Convenience macros for common allocations.
 * @{
 */

/**
 * @brief Allocate a parse tree node from the arena.
 * @return Newly allocated tnode_t, or nullptr on failure
 *
 * Nodes are allocated from the parse arena for efficient backtracking.
 * Use parse_arena_mark()/parse_arena_reset() around speculative parsing.
 */
tnode_t *alloc_tnode(void);

/** @} */
