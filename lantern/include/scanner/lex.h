#pragma once

#include "scanner/scanner.h"
#include "parse/grammar.h"

typedef enum : uint8_t {
    SCAN_LEX_LINE = 0,
    SCAN_LEX_BLOCK,
} scan_lex_comment_kind_t;

typedef struct {
    scan_lex_comment_kind_t kind;
    core_string_t           opener;
    core_string_t           closer;
    core_string_t           tok;
    bool                   nested;
} scan_lex_comment_t;

typedef enum : uint8_t {
    SCAN_LEX_STR_RAW         = 1 << 0,
    SCAN_LEX_STR_MULTILINE   = 1 << 1,
    SCAN_LEX_STR_HOLE_TOKENS = 1 << 2,
} scan_lex_str_flag;

typedef struct {
    core_string_t opener;
    core_string_t closer;
    core_string_t escape;
    core_string_t interp_open;
    core_string_t interp_close;
    core_string_t tok;
    core_string_t tok_mid;
    core_string_t tok_end;
    uint8_t      flags;
} scan_lex_string_t;

typedef struct {
    core_string_t prefix;
    core_string_t digits;
    core_string_t tok;
} scan_lex_radix_t;

typedef struct {
    core_string_t suffix;
    core_string_t tok;
} scan_lex_suffix_t;

typedef struct {
    const scan_lex_radix_t  *radixes;
    const scan_lex_suffix_t *suffixes;
    core_string_t            sep;
    core_string_t            sign;
    core_string_t            exponent;
    core_string_t            tok_int;
    core_string_t            tok_float;
} scan_lex_number_t;

typedef struct {
    core_string_t prefix;
    core_string_t tok;
    bool         digits_only;
} scan_lex_sigil_t;

typedef struct {
    core_string_t opener;
    core_string_t closer;
    core_string_t suffix_chars;
    core_string_t tok;
} scan_lex_regex_t;

typedef struct {
    core_string_t opener;
    core_string_t tok;
    bool         indent_strip;
} scan_lex_heredoc_t;

typedef struct {
    core_string_t open;
    core_string_t close;
    core_string_t tok;
    core_string_t open_tok;
    core_string_t close_tok;
} scan_lex_embed_t;

typedef struct {
    core_string_t open;
    core_string_t tok_eol;
    core_string_t tok_other;
} scan_lex_directive_t;

typedef enum : uint8_t {
    SCAN_LEX_LAYOUT_NONE    = 0,
    SCAN_LEX_LAYOUT_NEWLINE = 1 << 0,
    SCAN_LEX_LAYOUT_INDENT  = 1 << 1,
} scan_lex_layout_flag;

typedef struct {
    core_string_t name;

    const scan_lex_comment_t *comments;
    const scan_lex_string_t  *strings;
    const scan_lex_sigil_t   *sigils;
    const scan_lex_regex_t   *regexes;
    const scan_lex_heredoc_t *heredocs;
    const scan_lex_embed_t   *embeds;

    scan_lex_number_t number;

    uint8_t      layout;
    core_string_t tok_newline;
    core_string_t tok_terminator;
    core_string_t terminator_after;
    core_string_t tok_indent;
    core_string_t tok_dedent;
    core_string_t tok_eof;
    core_string_t tok_ident;

    core_string_t line_continue;
    core_string_t brackets;

    const scan_lex_directive_t *directives;

    core_string_t ident_start_extra;
    core_string_t ident_extra;
    core_string_t tok_ident_upper;
} scan_lex_spec_t;

typedef struct scan_lex_t scan_lex_t;

extern scan_lex_t *scan_lex_compile(const scan_lex_spec_t *spec,
                                  parse_grammar_t        *g);
extern void       scan_lex_free(scan_lex_t *lex);

extern bool scan_lex_scan(parse_scanner_t *s);
extern void scan_lex_reset_state(parse_scanner_t *s);

extern size_t scan_lex_operator_width(const scan_lex_t *lex, char first);

extern bool                  scan_lex_register(const scan_lex_spec_t *spec);
extern const scan_lex_spec_t *scan_lex_for(core_string_t name);

extern const scan_lex_spec_t *scan_lex_spec_from_grammar(parse_grammar_t *g);
extern void                  scan_lex_spec_free(const scan_lex_spec_t *spec);
extern const char           *scan_lex_build_error(void);

extern bool scan_grammar_set_lex(parse_grammar_t *g, const scan_lex_spec_t *spec);
extern const scan_lex_spec_t *scan_grammar_lex(parse_grammar_t *g);
extern scan_lex_t            *scan_grammar_lex_compiled(parse_grammar_t *g);
