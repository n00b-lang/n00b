#pragma once

#include "parse/grammar.h"
#include "parse/tree.h"
#include "lib/module.h"
#include "lib/buffer.h"

typedef bool (*n00bb_preprocessor_import_fn)(
    void *context, parse_grammar_t *grammar, parse_tree_t *import_statement,
    core_translation_unit_t *importer, core_translation_unit_t **imported,
    parse_tree_t **imported_tree);
typedef bool (*n00bb_preprocessor_prepare_import_fn)(
    void *context, parse_grammar_t *grammar, parse_tree_t *import_statement,
    core_translation_unit_t *importer);

typedef enum {
    N00BB_PREPROCESSOR_VALUE_BOOL,
    N00BB_PREPROCESSOR_VALUE_INT,
    N00BB_PREPROCESSOR_VALUE_FLOAT,
    N00BB_PREPROCESSOR_VALUE_STRING,
} n00bb_preprocessor_value_kind_t;

typedef struct {
    n00bb_preprocessor_value_kind_t kind;
    union {
        bool boolean;
        __int128 integer;
        double real;
        core_string_t string;
    } as;
} n00bb_preprocessor_value_t;

typedef struct {
    const char *const *definitions;
    size_t definition_count;
    n00bb_preprocessor_import_fn load_import;
    void *import_context;
    n00bb_preprocessor_prepare_import_fn prepare_import;
} n00bb_preprocessor_options_t;

typedef struct n00bb_preprocessor_t n00bb_preprocessor_t;

n00bb_preprocessor_t *n00bb_preprocessor_new(
    const n00bb_preprocessor_options_t *options);
void n00bb_preprocessor_free(n00bb_preprocessor_t *preprocessor);
bool n00bb_preprocessor_run(n00bb_preprocessor_t *preprocessor,
                           parse_grammar_t *grammar, parse_tree_t *tree,
                           core_translation_unit_t *unit);
bool n00bb_decode_string_literal(core_string_t raw, core_buffer_t *decoded);

bool n00bb_preprocess(parse_grammar_t *grammar, parse_tree_t *tree,
                      core_translation_unit_t *unit,
                      const n00bb_preprocessor_options_t *options);
bool n00bb_preprocessor_builtin_name(core_string_t name);
