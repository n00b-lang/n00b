#pragma once
/** @file md_render.h
 *  @brief Render a markdown AST into a styled `n00b_string_t`.
 *
 *  Walks the tree produced by `n00b_parse_markdown()` and maps
 *  markdown spans to abstract text styles:
 *
 *  | Markdown element          | Style applied              |
 *  |---------------------------|----------------------------|
 *  | `**strong**`              | bold                       |
 *  | `*emphasis*`              | italic                     |
 *  | `` `code` ``              | font_hint = MONO           |
 *  | `~~strikethrough~~`       | strikethrough              |
 *  | `<u>underline</u>`        | underline                  |
 *  | `# heading`               | bold (via heading role)    |
 *
 *  No ANSI codes or color resolution — output is an abstract styled
 *  string suitable for further processing by a presentation layer.
 *
 *  ### Related modules
 *
 *  - `strings/markdown.h` -- markdown parsing (produces the AST)
 *  - `strings/string_style.h` -- style attachment API
 *  - `strings/text_style.h` -- style type definitions
 */

#include "text/strings/markdown.h"
#include "text/strings/string_style.h"

// ===================================================================
// Public API
// ===================================================================

/** @brief Render a markdown AST into a single styled string.
 *
 *  Recursively walks the AST, concatenating text nodes and applying
 *  styles based on the enclosing span/block context.  The result is
 *  a single `n00b_string_t` with style records for each styled region.
 *
 *  @param tree  Root of a parsed markdown AST.
 *  @return Styled `n00b_string_t *`.
 *
 *  @pre @p tree was produced by `n00b_parse_markdown()`.
 *  @post Returned string has style records for bold, italic, code,
 *        strikethrough, and underline spans.
 */
n00b_string_t *
n00b_str_md_render(n00b_tree_t(n00b_md_node_t, n00b_md_node_t) *tree);
