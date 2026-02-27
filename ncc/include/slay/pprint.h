#pragma once

/**
 * @file pprint.h
 * @brief Code pretty-printing (tree to formatted source) and BNF emission.
 *
 * Two-phase Wadler/Lindig approach:
 * Phase 1: Tree walk -> document command stream
 * Phase 2: Layout resolution -> formatted output
 *
 * Annotations on NTs control formatting: @indent, @group, @concat,
 * @blankline, @softline($N), @hardline($N), @nospace($N), @align($N).
 */

#include "slay/grammar.h"
#include "slay/parse_tree.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// ============================================================================
// Document command types (public for advanced users)
// ============================================================================

typedef enum {
    N00B_DOC_TEXT,
    N00B_DOC_SPACE,
    N00B_DOC_SOFTLINE,
    N00B_DOC_HARDLINE,
    N00B_DOC_BLANKLINE,
    N00B_DOC_INDENT,
    N00B_DOC_DEDENT,
    N00B_DOC_GROUP_BEGIN,
    N00B_DOC_GROUP_END,
    N00B_DOC_ALIGN_BEGIN,
    N00B_DOC_ALIGN_END,
} n00b_doc_kind_t;

// ============================================================================
// Per-NT formatting rule (for style tables)
// ============================================================================

typedef struct {
    const char *nt_name;
    bool        indent;
    bool        group;
    bool        concat;
    bool        blankline_after;
    int32_t    *hardline_before;  // -1 terminated array
    int32_t    *softline_before;  // -1 terminated array
    int32_t    *nospace_before;   // -1 terminated array
    int32_t    *align_to;         // -1 terminated array
} n00b_pprint_rule_t;

typedef n00b_pprint_rule_t *n00b_pprint_style_t;

// ============================================================================
// Options
// ============================================================================

typedef enum {
    N00B_PPRINT_SPACES,
    N00B_PPRINT_TABS,
} n00b_indent_style_t;

typedef struct {
    int32_t              line_width;
    int32_t              indent_size;
    n00b_indent_style_t  indent_style;
    bool                 use_unicode_width;
    FILE                *out;
    const char          *newline;
    n00b_pprint_style_t  style;
} n00b_pprint_opts_t;

// ============================================================================
// Code pretty-printing (tree -> formatted source)
// ============================================================================

/**
 * @brief Pretty-print a parse tree into formatted source.
 * @param g     Grammar for NT name resolution and annotation lookup.
 * @param tree  Root of the parse tree.
 * @param opts  Pretty-printer options.
 * @return Heap-allocated string (caller frees), or NULL if opts.out was set.
 */
char *n00b_pprint(n00b_grammar_t    *g,
                   n00b_parse_tree_t *tree,
                   n00b_pprint_opts_t opts);

// ============================================================================
// BNF emission (grammar -> BNF text, with annotations)
// ============================================================================

/**
 * @brief Emit a grammar as BNF text.
 * @param g     Grammar to emit.
 * @param opts  Pretty-printer options for formatting the output.
 * @return Heap-allocated string (caller frees), or NULL if opts.out was set.
 */
char *n00b_grammar_emit_bnf(n00b_grammar_t    *g,
                              n00b_pprint_opts_t opts);
