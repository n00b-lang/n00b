#pragma once

#include "parse/grammar.h"
#include "parse/tree.h"
#include <stdio.h>

core_string_t parse_tree_dump_str(parse_grammar_t *g, parse_tree_t *tree, bool raw);

void parse_tree_dump(parse_grammar_t *g, parse_tree_t *tree,
                   FILE *out, bool raw);
