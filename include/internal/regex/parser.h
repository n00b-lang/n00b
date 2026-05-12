/**
 * @file parser.h
 * @brief Parser for resharp regex patterns.
 *
 * Converts regex pattern strings into the algebra `NodeId` representation
 * used by the regex builder.  Faithful translation of upstream Rust
 * resharp-parser; primitives translated to n00b idioms (allocate-by-type,
 * `n00b_require` / `n00b_panic` / `n00b_unreachable`, no libc shims).
 *
 * The names here keep the upstream Rust algorithmic vocabulary
 * (`PatternFlags`, `ResharpParser`, `ParseError`, `parser_*`).  This file
 * is internal to the regex engine; nothing here is part of the public
 * n00b surface.
 *
 * Companion source: `src/text/regex/parser/parser.c`.
 *
 * Cross-file dependencies:
 *   - algebra.h (Phase 5)  — `RegexBuilder`, `NodeId`, `regex_builder_mk_*`.
 *   - ast.h     (Phase 6)  — `ast_Ast`, `ast_ErrorKind_tag`, `rs_*` types.
 *   - ids.h     (Phase 3)  — `NodeId` newtype.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "internal/regex/algebra.h"
#include "internal/regex/unicode_classes_mod.h"
#include "internal/regex/ast.h"

// ---------------------------------------------------------------------------
// Compile-time limits.
// ---------------------------------------------------------------------------

/** @brief Maximum count permitted in `{m,n}` repetition operators. */
constexpr uint32_t REPETITION_COUNT_LIMIT = 2000u;

/** @brief Default expansion-budget cap on the parsed AST. */
constexpr uint64_t DEFAULT_EXPANDED_AST_LIMIT = 50000ull;

/** @brief Default maximum concat list length. */
constexpr size_t   DEFAULT_MAX_LIST_LEN = 4000;

// ---------------------------------------------------------------------------
// PatternFlags — mode bits controlling parser behaviour.
// ---------------------------------------------------------------------------

/**
 * @brief Per-parse flags.  Mirrors upstream Rust `PatternFlags`.
 */
typedef struct PatternFlags {
    bool     unicode;
    bool     full_unicode;
    bool     case_insensitive;
    bool     dot_matches_new_line;
    bool     multiline;
    bool     ignore_whitespace;
    bool     ascii_perl_classes;
    uint64_t expanded_ast_limit;
    size_t   max_list_len;
} PatternFlags;

/** @brief Default flags: unicode = true, multiline = true, the rest false/defaults. */
PatternFlags PatternFlags_default(void);

// ---------------------------------------------------------------------------
// ParseError — diagnostic for a failed parse.
// ---------------------------------------------------------------------------

/**
 * @brief Heap-owned parse-error payload.
 *
 * `pattern` is heap-owned by `n00b_alloc_array` and freed by
 * `ParseError_free`.  `kind` carries the discriminator and (for the
 * Duplicate / RepeatedNegation kinds) the original span.
 */
typedef struct ParseError {
    ast_ErrorKind kind;
    char         *pattern;  // n00b-alloc'd, NUL-terminated
    rs_Span       span;
} ParseError;

/** @brief Drop a `ParseError`, freeing its heap-owned `pattern` field. */
void ParseError_free(ParseError *e);

// ---------------------------------------------------------------------------
// Either<L, R> — small tagged union used by parse_group's two-armed result.
// ---------------------------------------------------------------------------

typedef enum {
    EITHER_LEFT,
    EITHER_RIGHT,
} Either_tag;

// ---------------------------------------------------------------------------
// WordCharKind — internal vocabulary for the word-boundary rewriter.
// ---------------------------------------------------------------------------

typedef enum {
    WCK_WORD,
    WCK_NON_WORD,
    WCK_MAYBE_WORD,
    WCK_MAYBE_NON_WORD,
    WCK_UNKNOWN,
    WCK_EDGE,
} WordCharKind;

// ---------------------------------------------------------------------------
// Public free functions.
// ---------------------------------------------------------------------------

/** @brief Whether @p c is a regex meta-character that needs escaping. */
bool parser_is_meta_character(uint32_t c);

/** @brief Whether @p c is a character that may legally appear after `\\`. */
bool parser_is_escapeable_character(uint32_t c);

/**
 * @brief Allocate a NUL-terminated copy of @p text with meta-characters escaped.
 *
 * Returned buffer is heap-owned (n00b allocator); caller must `n00b_free` it.
 */
char *parser_escape(const char *text);

/**
 * @brief Append the escaped form of @p text to a caller-owned growable byte
 *        buffer.  The buffer is grown via the n00b allocator; ownership of
 *        the storage transfers to the caller (see `parser_escape`).
 *
 * @kw text     null-terminated input.
 * @kw buf      in/out pointer to the heap-owned char buffer.
 * @kw buf_len  in/out length of valid bytes in *buf (excludes terminator).
 * @kw buf_cap  in/out allocated capacity of *buf.
 */
void parser_escape_into(const char *text, char **buf,
                        size_t *buf_len, size_t *buf_cap);

// ---------------------------------------------------------------------------
// ResharpParser — opaque parser state.
// ---------------------------------------------------------------------------

typedef struct ResharpParser ResharpParser;

/** @brief Construct a parser over @p pattern using `PatternFlags_default()`. */
ResharpParser *resharp_parser_new(const char *pattern);

/** @brief Construct a parser over @p pattern using the supplied flags. */
ResharpParser *resharp_parser_with_flags(const char *pattern,
                                         const PatternFlags *flags);

/** @brief Drop a parser.  Safe with @p p == nullptr. */
void           resharp_parser_free(ResharpParser *p);

// ---------------------------------------------------------------------------
// AST size limits — used by the parser to trip oversized patterns.
// ---------------------------------------------------------------------------

/** @brief Max single-concatenation length anywhere in the AST. */
size_t   max_concat_length(const ast_Ast *ast);

/** @brief Saturating upper bound on the expanded-AST node count. */
uint64_t expanded_ast_size(const ast_Ast *ast, uint64_t limit);

// ---------------------------------------------------------------------------
// Public top-level parse helpers.
// ---------------------------------------------------------------------------

/**
 * @brief Parse @p pattern into the algebra DAG owned by @p tb.
 *
 * On success returns the root `NodeId` and writes nullptr to @p *err.
 * On failure returns `NODE_ID_BOT` and writes a heap-owned `ParseError *`
 * to @p *err which the caller must drop via `ParseError_free` + `n00b_free`.
 */
NodeId   parser_parse_ast(RegexBuilder *tb, const char *pattern,
                          ParseError **err);

/** @brief Like `parser_parse_ast` but with explicit flags. */
NodeId   parser_parse_ast_with(RegexBuilder *tb, const char *pattern,
                               const PatternFlags *flags,
                               ParseError **err);

/**
 * @brief Parse @p pattern to its raw `ast_Ast *` form (no algebra lowering).
 *
 * Returns nullptr on parse error and writes the diagnostic to @p *err.
 * Caller owns the returned AST and must drop it via `ast_Ast_free`.
 */
ast_Ast *parser_parse_to_ast(const char *pattern, ParseError **err);

/**
 * @brief Test-friendly convenience entry point.
 *
 * Returns true on success and writes the resulting `NodeId` via
 * @p *out_node; returns false on parse error and discards the diagnostic.
 */
bool     resharp_parser_parse_ast(RegexBuilder *tb, const char *pattern,
                                  NodeId *out_node);

/**
 * @brief Engine-test-shape entry point: returns `NodeId` (`NODE_ID_BOT` on
 *        failure) and writes 0 to @p *err_out on success, nonzero on error.
 *
 * Test code treats 0 as success and any nonzero value as a parse error;
 * the parser does not depend on the engine's `ErrorKind` enum.
 */
NodeId   resharp_parse_ast(RegexBuilder *tb, const char *pattern,
                           int *err_out);
