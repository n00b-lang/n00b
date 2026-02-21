#pragma once

/**
 * @file token.h
 * @brief Token types, trivia, and token list for the slay parser system.
 *
 * Tokens are the universal currency — every tokenizer produces them,
 * every parser consumes them, parse trees reference them.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "core/alloc.h"
#include "core/array.h"
#include "core/list.h"
#include "core/string.h"

// ============================================================================
// Constants
// ============================================================================

/** @brief Base token ID offset for user-registered terminals. */
#define N00B_TOK_START_ID   0x40000000

/** @brief Token ID for unrecognized input. */
#define N00B_TOK_OTHER      (-3)

/** @brief Token ID for whitespace/comments that should be skipped. */
#define N00B_TOK_IGNORED    (-2)

/** @brief Token ID indicating end of input. */
#define N00B_TOK_EOF        (-1)

/** @brief Default token ID for identifiers (first registered terminal). */
#define N00B_TOK_IDENTIFIER   (N00B_TOK_START_ID + 1)

/** @brief Token ID for typedef/type names (reclassified identifiers). */
#define N00B_TOK_TYPEDEF_NAME (N00B_TOK_START_ID + 2)

/** @brief Token ID for integer literals. */
#define N00B_TOK_INTEGER      (N00B_TOK_START_ID + 3)

/** @brief Token ID for floating-point literals. */
#define N00B_TOK_FLOAT        (N00B_TOK_START_ID + 4)

/** @brief Token ID for character literals. */
#define N00B_TOK_CHAR_LIT     (N00B_TOK_START_ID + 5)

/** @brief Token ID for string literals. */
#define N00B_TOK_STRING_LIT   (N00B_TOK_START_ID + 6)

// ============================================================================
// Trivia
// ============================================================================

/** @brief A piece of trivia (whitespace, comment) attached to a token. */
typedef struct n00b_trivia_t {
    n00b_string_t         text; /**< Trivia text (GC-managed data). */
    struct n00b_trivia_t *next; /**< Next trivia piece in the linked list. */
} n00b_trivia_t;

// ============================================================================
// Token
// ============================================================================

n00b_option_decl(n00b_string_t);

/** @brief Token with position, value, trivia, and user data. */
typedef struct n00b_token_info_t {
    void                          *user_info;       /**< User-defined data. */
    n00b_option_t(n00b_string_t)   value;           /**< Token text (optional). */
    n00b_option_t(n00b_string_t)   file;            /**< Source file path (optional). */
    n00b_trivia_t                 *leading_trivia;  /**< Whitespace/comments before token. */
    n00b_trivia_t                 *trailing_trivia; /**< Line comment after token (same line). */
    int32_t                        tid;             /**< Terminal ID assigned by tokenizer. */
    int32_t                        index;           /**< Index in token array. */
    uint32_t                       line;            /**< 1-based source line. */
    uint32_t                       column;          /**< 1-based source column. */
    uint32_t                       endcol;          /**< 1-based end column. */
    bool                           system_header;   /**< True if from system header. */
} n00b_token_info_t;

// ============================================================================
// Token list
// ============================================================================

n00b_list_decl(n00b_token_info_t);

typedef n00b_token_info_t *n00b_token_info_ptr_t;
n00b_array_decl(n00b_token_info_ptr_t);
n00b_list_decl(n00b_token_info_ptr_t);

/**
 * @brief Build a pointer array from a token list (for parser consumption).
 *
 * @param tl    Token list.
 * @param out   Receives pointer to array of `n00b_token_info_t *`.
 * @return      Number of tokens.
 */
extern int32_t n00b_token_list_build_ptrs(n00b_list_t(n00b_token_info_t) *tl,
                                           n00b_token_info_ptr_t **out);

// ============================================================================
// Tokenizer callback
// ============================================================================

// Forward declaration — parser type defined later.
typedef struct n00b_parser_t n00b_parser_t;

/** @brief Tokenizer callback that returns the next token ID. */
typedef int64_t (*n00b_tokenizer_fn)(n00b_parser_t *, void **);
