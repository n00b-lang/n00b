#pragma once

#include "parse/annotation.h"
#include "parse/annot_walk.h"


bool ncc_annot_parse_args(ncc_annotation_t *a, ncc_string_t text);

ncc_string_t ncc_annot_arg_grammar(ncc_annot_kind_t kind);

void ncc_annot_grammar_shutdown(void);
