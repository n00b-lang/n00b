#pragma once
/** @file md_lines.h
 *  @brief Convert a markdown AST into an array of styled lines.
 *
 *  Each element in the returned array is one renderable unit:
 *  a paragraph, heading, list item, code line, or separator.
 *  No layout or line-wrapping is performed — the caller is
 *  responsible for reflowing text to a target width.
 *
 *  ### Mapping
 *
 *  | AST node            | Array entry                          |
 *  |---------------------|--------------------------------------|
 *  | Paragraph           | One entry with inline styles         |
 *  | Heading             | One entry with bold style            |
 *  | List item           | One entry per item (bullet prefix)   |
 *  | Code block          | One entry per source line (mono)     |
 *  | Horizontal rule     | One entry containing `"---"`         |
 *
 *  ### Related modules
 *
 *  - `strings/md_render.h` -- single styled string from AST
 *  - `strings/markdown.h` -- markdown parsing
 *  - `core/list.h` -- `n00b_list_t` (used internally)
 *  - `core/array.h` -- `n00b_array_t` (return type)
 */

#include "text/strings/markdown.h"
#include "text/strings/string_style.h"
#include "text/strings/string_ops.h"
#include "adt/list.h"

// ===================================================================
// Public API
// ===================================================================

/** @brief Convert a markdown AST into an array of styled lines.
 *
 *  Walks the tree produced by `n00b_parse_markdown()` and emits one
 *  `n00b_string_t` per renderable unit (paragraph, heading, list item,
 *  code line, or horizontal rule).  Inline styles (bold, italic, code,
 *  strikethrough, underline) are attached to the appropriate ranges.
 *
 *  @param tree  Root of a parsed markdown AST.
 *  @return Array of styled strings.  Caller frees with
 *          `n00b_array_free()`.
 *
 *  @pre @p tree was produced by `n00b_parse_markdown()`.
 *  @post Each array element is a self-contained styled string.
 */
n00b_array_t(n00b_string_t)
n00b_str_md_to_lines(n00b_tree_t(n00b_md_node_t, n00b_md_node_t) *tree);
