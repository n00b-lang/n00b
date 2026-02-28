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
#include "adt/array.h"
#include "adt/list.h"
#include "core/string.h"

// ============================================================================
// Constants
// ============================================================================

/** @brief Token ID for unrecognized input (sentinel). */
#define N00B_TOK_OTHER      (-3LL)

/** @brief Token ID for whitespace/comments that should be skipped. */
#define N00B_TOK_IGNORED    (-2LL)

/** @brief Token ID indicating end of input. */
#define N00B_TOK_EOF        (-1LL)

// ============================================================================
// Hash-based token ID helpers
// ============================================================================

#define XXH_INLINE_ALL
#include "vendor/xxhash.h"

/**
 * @brief Compute a fixed-text terminal ID from its text.
 *
 * Uses XXH3_64bits with the MSB set so the result is always negative
 * when interpreted as int64_t, and less than N00B_TOK_OTHER (-3).
 *
 * Grammar registration and scanner emission independently produce the
 * same ID for the same text — no coordination needed.
 */
static inline int64_t
n00b_token_id_from_text(const char *text, size_t len)
{
    return (int64_t)(XXH3_64bits(text, len) | (1ULL << 63));
}

/**
 * @brief True if a token ID is a hash-based fixed-text terminal.
 *
 * Fixed-text IDs are always < N00B_TOK_OTHER (-3) because the MSB
 * is set by `n00b_token_id_from_text`.
 */
static inline bool
n00b_token_id_is_fixed_text(int64_t tid)
{
    return tid < N00B_TOK_OTHER;
}

// ============================================================================
// Token emission error codes
// ============================================================================

/**
 * @brief Error codes returned by `n00b_scan_emit()`.
 */
typedef enum {
    N00B_TOK_OK                 = 0,   /**< Token emitted successfully. */
    N00B_TOK_ERR_NO_TEXT        = -1,  /**< No text: no contents and no mark. */
    N00B_TOK_ERR_BAD_ARGS       = -2,  /**< Invalid argument combination. */
    N00B_TOK_ERR_NOT_IN_GRAMMAR = -3,  /**< Hashed text not in grammar. */
    N00B_TOK_ERR_BAD_TYPE_NAME  = -4,  /**< token_type name not registered. */
} n00b_token_err_t;

// ============================================================================
// Trivia
// ============================================================================

/** @brief A piece of trivia (whitespace, comment) attached to a token. */
typedef struct n00b_trivia_t {
    n00b_string_t        *text; /**< Trivia text (GC-managed data). */
    struct n00b_trivia_t *next; /**< Next trivia piece in the linked list. */
} n00b_trivia_t;

// ============================================================================
// Token
// ============================================================================

// n00b_option_t(n00b_string_t *) is declared in core/string.h (included above).

/** @brief Token with position, value, trivia, and user data. */
typedef struct n00b_token_info_t {
    void                            *user_info;       /**< User-defined data. */
    n00b_option_t(n00b_string_t *)   value;           /**< Token text (optional). */
    n00b_option_t(n00b_string_t *)   file;            /**< Source file path (optional). */
    n00b_option_t(n00b_string_t *)   modifier;        /**< Literal modifier (e.g., 'hex). */
    n00b_trivia_t                 *leading_trivia;  /**< Whitespace/comments before token. */
    n00b_trivia_t                 *trailing_trivia; /**< Line comment after token (same line). */
    int64_t                        tid;             /**< Terminal ID assigned by tokenizer. */
    int32_t                        index;           /**< Index in token array. */
    uint32_t                       line;            /**< 1-based source line. */
    uint32_t                       column;          /**< 1-based source column. */
    uint32_t                       endcol;          /**< 1-based end column. */
    bool                           system_header;   /**< True if from system header. */
} n00b_token_info_t;

// ============================================================================
// Token list
// ============================================================================

typedef n00b_token_info_t *n00b_token_info_ptr_t;

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
