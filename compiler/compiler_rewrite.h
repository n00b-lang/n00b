#ifndef N00BB_COMPILER_REWRITE_H
#define N00BB_COMPILER_REWRITE_H

#include "compiler_link.h"
#include "compiler_types.h"

bool n00bb_rewrite_typed_ir(n00bb_link_analysis_t *analysis,
                            parse_grammar_t *grammar, n00bb_typing_t *typing);

#endif
