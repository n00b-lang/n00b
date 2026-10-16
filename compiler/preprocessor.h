#pragma once

#include "parse/grammar.h"
#include "parse/tree.h"
#include "lib/module.h"
#include "lib/buffer.h"

typedef bool (*n00bb_preprocessor_import_fn)(
    void *context, parse_grammar_t *grammar, parse_tree_t *import_statement,
    core_translation_unit_t *importer, core_translation_unit_t **imported,
    parse_tree_t **imported_tree);
typedef bool (*n00bb_preprocessor_implicit_import_fn)(
    void *context, parse_grammar_t *grammar, core_translation_unit_t *importer,
    core_string_t module_name);
typedef bool (*n00bb_preprocessor_prepare_import_fn)(
    void *context, parse_grammar_t *grammar, parse_tree_t *import_statement,
    core_translation_unit_t *importer);
typedef bool (*n00bb_preprocessor_selected_body_fn)(
    void *context, core_translation_unit_t *unit, parse_tree_t *body);
typedef bool (*n00bb_preprocessor_module_literal_fn)(
    void *context, parse_grammar_t *grammar, core_translation_unit_t *importer,
    parse_tree_t *literal, core_string_t modifier, core_string_t name,
    core_translation_unit_t **module, parse_tree_t **module_tree);
typedef bool (*n00bb_preprocessor_litmod_fn)(
    void *context, core_translation_unit_t *unit,
    parse_tree_t *declaration, core_string_t modifier, core_string_t kind);

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
    bool release_mode;
    bool build_time;
    n00bb_preprocessor_import_fn load_import;
    n00bb_preprocessor_implicit_import_fn load_implicit_import;
    void *import_context;
    n00bb_preprocessor_prepare_import_fn prepare_import;
    n00bb_preprocessor_selected_body_fn selected_body;
    n00bb_preprocessor_selected_body_fn build_time_use;
    void *selected_body_context;
    n00bb_preprocessor_litmod_fn register_litmod;
    void *litmod_context;
    const core_string_t *module_modifiers;
    size_t module_modifier_count;
    n00bb_preprocessor_module_literal_fn load_module_literal;
    const void *module_literal_context;
} n00bb_preprocessor_options_t;

typedef struct n00bb_preprocessor_t n00bb_preprocessor_t;

n00bb_preprocessor_t *n00bb_preprocessor_new(
    const n00bb_preprocessor_options_t *options);
void n00bb_preprocessor_free(n00bb_preprocessor_t *preprocessor);
bool n00bb_preprocessor_run(n00bb_preprocessor_t *preprocessor,
                           parse_grammar_t *grammar, parse_tree_t *tree,
                           core_translation_unit_t *unit);
bool n00bb_decode_string_literal(core_string_t raw, core_buffer_t *decoded);
bool n00bb_decode_char_literal(core_string_t raw, uint32_t *scalar);

bool n00bb_preprocessor_builtin_name(core_string_t name);
