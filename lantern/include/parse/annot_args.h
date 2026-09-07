#pragma once

#include "parse/annotation.h"
#include "parse/annot_walk.h"


bool parse_annot_parse_args(parse_annotation_t *a, core_string_t text);

core_string_t parse_annot_arg_grammar(parse_annot_kind_t kind);

void parse_annot_grammar_shutdown(void);
