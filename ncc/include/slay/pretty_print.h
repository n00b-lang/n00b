#pragma once

#include "slay/grammar.h"
#include "slay/parse_tree.h"
#include <stdio.h>

/**
 * @brief Print a parse tree to a file stream.
 * @param g     Grammar for NT name resolution.
 * @param tree  Root parse tree node.
 * @param out   Output FILE stream.
 * @param raw   If true, show group wrapper nodes instead of flattening them.
 */
void n00b_parse_tree_print(n00b_grammar_t *g, n00b_parse_tree_t *tree,
                            FILE *out, bool raw);
