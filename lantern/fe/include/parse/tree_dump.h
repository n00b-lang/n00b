#pragma once

#include "parse/grammar.h"
#include "parse/parse_tree.h"
#include <stdio.h>

void ncc_tree_dump(ncc_grammar_t *g, ncc_parse_tree_t *tree,
                   FILE *out, bool raw);
