#pragma once

#include "parse/grammar.h"
#include "parse/tree.h"
#include "lib/module.h"

typedef bool (*n00bb_preprocessor_import_fn)(
    void *context, parse_grammar_t *grammar, parse_tree_t *import_statement,
    core_translation_unit_t *importer, core_translation_unit_t **imported,
    parse_tree_t **imported_tree);
typedef bool (*n00bb_preprocessor_prepare_import_fn)(
    void *context, parse_grammar_t *grammar, parse_tree_t *import_statement,
    core_translation_unit_t *importer);

typedef void (*n00bb_preprocessor_node_fn)(
    void *context, core_translation_unit_t *unit, parse_tree_t *node,
    parse_tree_t *parent, int32_t depth, int32_t child_index, bool entering);

typedef struct {
    const char *const *definitions;
    size_t definition_count;
    n00bb_preprocessor_import_fn load_import;
    void *import_context;
    n00bb_preprocessor_prepare_import_fn prepare_import;
    n00bb_preprocessor_node_fn visit_node;
    void *visit_context;
} n00bb_preprocessor_options_t;

typedef struct n00bb_preprocessor_t n00bb_preprocessor_t;

n00bb_preprocessor_t *n00bb_preprocessor_new(
    const n00bb_preprocessor_options_t *options);
void n00bb_preprocessor_free(n00bb_preprocessor_t *preprocessor);
bool n00bb_preprocessor_run(n00bb_preprocessor_t *preprocessor,
                           parse_grammar_t *grammar, parse_tree_t *tree,
                           core_translation_unit_t *unit);

bool n00bb_preprocess(parse_grammar_t *grammar, parse_tree_t *tree,
                      core_translation_unit_t *unit,
                      const n00bb_preprocessor_options_t *options);
