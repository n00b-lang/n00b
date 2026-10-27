#pragma once

#include "parse/symtab.h"
#include "parse/tree.h"
#include "th/typehash.h"

#include "parse/grammar.h"

th_type *n00bb_narrowed_symbol_type(th_ctx *types, parse_sym_entry_t *symbol,
                                    parse_tree_t *use, th_type *declared,
                                    void *context);
parse_tree_t *n00bb_narrowing_ended_by(parse_grammar_t *grammar,
                                       parse_tree_t *use);
