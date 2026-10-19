#pragma once

#include "lib/list.h"
#include "lib/string.h"
#include "parse/types.h"
#include "scanner/lex.h"

#define SCAN_LEX_DIRECTIVES(X) \
    X(COMMENT, "lex-comment") \
    X(REGION, "lex-region") \
    X(STRING, "lex-string") \
    X(STRING_FAMILY, "lex-string-family") \
    X(STRING_ESCAPES, "lex-string-escapes") \
    X(NUMBER, "lex-number") \
    X(NUMBER_SUFFIXES, "lex-number-suffixes") \
    X(LAYOUT, "lex-layout") \
    X(SIGIL, "lex-sigil") \
    X(REGEX, "lex-regex") \
    X(HEREDOC_HEADER, "lex-heredoc-header") \
    X(HEREDOC, "lex-heredoc") \
    X(IDENT, "lex-ident") \
    X(IDENT_PROFILE, "lex-ident-profile") \
    X(EMBED, "lex-embed") \
    X(FENCE, "lex-fence") \
    X(USE, "lex-use") \
    X(OPERATOR_SET, "lex-operator-set") \
    X(OPERATORS, "lex-operators") \
    X(WHITESPACE, "lex-whitespace") \
    X(FALLBACK, "lex-fallback") \
    X(DIRECTIVE, "lex-directive") \
    X(LITERAL_HOOK, "lex-literal-hook") \
    X(RECOVER, "lex-recover") \
    X(SOFT_KEYWORDS, "lex-soft-keywords")

typedef enum {
#define SCAN_LEX_ENUM(name, spelling) SCAN_LEX_##name,
    SCAN_LEX_DIRECTIVES(SCAN_LEX_ENUM)
#undef SCAN_LEX_ENUM
    SCAN_LEX_KIND_COUNT,
} scan_lex_kind_t;

typedef struct {
    core_string_t name;
    core_list_t(core_string_t) args;
} scan_lex_opt_t;

core_list_decl(scan_lex_opt_t);

typedef struct {
    scan_lex_kind_t kind;
    core_string_t source_origin;
    core_list_t(scan_lex_opt_t) lex_opts;
} scan_lex_decl_t;

core_list_decl(scan_lex_decl_t);

bool scan_lex_parse_file(core_string_t text, core_string_t origin,
                         core_list_t(scan_lex_decl_t) *decls);
void scan_lex_decls_free(core_list_t(scan_lex_decl_t) *decls);
core_string_t scan_lex_parse_error(void);
core_string_t scan_lex_kind_name(scan_lex_kind_t kind);
void scan_lex_note_unresolved_import(void);

const scan_lex_spec_t *scan_lex_spec_from_decls(
    parse_grammar_t *g, core_list_t(scan_lex_decl_t) *decls);
