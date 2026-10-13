#pragma once

#include "scanner/scanner.h"
#include "parse/grammar.h"

typedef struct {
    core_string_t spelling;
    core_string_t token;
    core_string_t modifier;
} scan_lex_literal_input_t;

typedef struct {
    bool          accepted;
    bool          cacheable;
    core_string_t borrowed_value;
    core_string_t borrowed_token;
    core_string_t borrowed_type;
    core_string_t borrowed_diagnostic;
    size_t        diagnostic_start;
    size_t        diagnostic_end;
} scan_lex_literal_borrowed_result_t;

typedef scan_lex_literal_borrowed_result_t
(*scan_lex_literal_hook_fn)(const scan_lex_literal_input_t *input);

typedef struct {
    bool          accepted;
    core_string_t value;
    core_string_t token;
    core_string_t type;
    core_string_t diagnostic;
    size_t        diagnostic_start;
    size_t        diagnostic_end;
} scan_lex_literal_result_t;

typedef struct {
    core_string_t        token;
    core_string_t        hook;
    core_string_t        modifier;
    const core_string_t *outputs;
    const core_string_t *types;
} scan_lex_literal_hook_t;

typedef enum : uint8_t {
    SCAN_LEX_LINE = 0,
    SCAN_LEX_BLOCK,
} scan_lex_comment_kind_t;

typedef struct {
    scan_lex_comment_kind_t kind;
    core_string_t           opener;
    core_string_t           closer;
    core_string_t           fill;
    core_string_t           tok;
    bool                   nested;
} scan_lex_comment_t;

typedef struct {
    core_string_t opener;
    core_string_t closer;
    core_string_t terminator;
} scan_lex_region_t;

typedef enum : uint8_t {
    SCAN_LEX_STR_RAW         = 1 << 0,
    SCAN_LEX_STR_MULTILINE   = 1 << 1,
    SCAN_LEX_STR_HOLE_TOKENS = 1 << 2,
    SCAN_LEX_STR_ONE_SCALAR  = 1 << 3,
    SCAN_LEX_STR_NESTED      = 1 << 4,
} scan_lex_str_flag;

typedef struct {
    core_string_t prefix;
    core_string_t digits;
    uint32_t      width;
} scan_lex_escape_t;

typedef struct {
    core_string_t opener;
    core_string_t closer;
    core_string_t nest_open;
    core_string_t escape;
    core_string_t interp_open;
    core_string_t interp_close;
    core_string_t tok;
    core_string_t tok_mid;
    core_string_t tok_end;
    core_string_t simple_escapes;
    const scan_lex_escape_t *escape_rules;
    uint8_t      flags;
} scan_lex_string_t;

typedef struct {
    core_string_t suffix;
    core_string_t tok;
    uint8_t       forms;
} scan_lex_suffix_t;

typedef enum : uint8_t {
    SCAN_LEX_SUFFIX_INTEGER = 1 << 0,
    SCAN_LEX_SUFFIX_FLOAT   = 1 << 1,
} scan_lex_suffix_form_t;

typedef enum : uint8_t {
    SCAN_LEX_NUM_POINT                    = 1 << 0,
    SCAN_LEX_NUM_LEADING_POINT            = 1 << 1,
    SCAN_LEX_NUM_TRAILING_POINT           = 1 << 2,
    SCAN_LEX_NUM_EXPONENT_REQUIRED_FOR_POINT = 1 << 3,
    SCAN_LEX_NUM_SEPARATOR_AFTER_PREFIX    = 1 << 4,
} scan_lex_number_flag_t;

typedef struct {
    core_string_t            prefix;
    core_string_t            digits;
    core_string_t            separator;
    core_string_t            exponent;
    core_string_t            exponent_digits;
    core_string_t            sign;
    core_string_t            tok_int;
    core_string_t            tok_float;
    const scan_lex_suffix_t  *suffixes;
    uint8_t                   flags;
} scan_lex_number_form_t;

typedef struct {
    core_string_t prefix;
    core_string_t tok;
    bool         digits_only;
    bool         before_string;
    bool         unless_closed;
    bool         attach_previous;
} scan_lex_sigil_t;

typedef struct {
    core_string_t opener;
    core_string_t closer;
    core_string_t suffix_chars;
    core_string_t tok;
    const core_string_t *not_after;
} scan_lex_regex_t;

typedef enum : uint8_t {
    SCAN_LEX_HEREDOC_UNQUOTED = 1 << 0,
    SCAN_LEX_HEREDOC_SINGLE_QUOTED = 1 << 1,
    SCAN_LEX_HEREDOC_DOUBLE_QUOTED = 1 << 2,
} scan_lex_heredoc_quote_t;

typedef struct {
    core_string_t name;
    core_string_t tag_start_extra;
    core_string_t tag_continue_extra;
    uint8_t       quotes;
    bool          leading_space;
    bool          trailing_text;
} scan_lex_heredoc_header_t;

typedef struct {
    core_string_t opener;
    core_string_t tok;
    core_string_t interpolated_tok;
    scan_lex_heredoc_header_t header;
    uint8_t       interpolate;
    bool          terminator_indent;
    bool          body_indent_strip;
    bool          queued;
} scan_lex_heredoc_t;

typedef struct {
    core_string_t open;
    core_string_t close;
    core_string_t tok;
    core_string_t open_tok;
    core_string_t close_tok;
    core_string_t lexer;
    bool          tokens;
} scan_lex_embed_t;

typedef enum : uint8_t {
    SCAN_LEX_FENCE_PAD_NONE,
    SCAN_LEX_FENCE_PAD_OPTIONAL,
    SCAN_LEX_FENCE_PAD_REQUIRED,
} scan_lex_fence_pad_t;

typedef struct {
    core_string_t outer_open;
    core_string_t outer_close;
    core_string_t fill;
    core_string_t body_open;
    core_string_t body_close;
    core_string_t tok;
    core_string_t pad_chars;
    uint32_t      minimum;
    bool          label;
    scan_lex_fence_pad_t pad;
} scan_lex_fence_t;

typedef struct {
    core_string_t open;
    core_string_t tok_eol;
    core_string_t tok_other;
} scan_lex_directive_t;

typedef struct {
    core_string_t       token;
    core_string_t       replacement;
    const core_string_t *synchronize;
    bool                consume;
} scan_lex_recovery_t;

typedef enum : uint8_t {
    SCAN_LEX_DECL_NONE,
    SCAN_LEX_DECL_COMMENT,
    SCAN_LEX_DECL_STRING,
    SCAN_LEX_DECL_NUMBER,
    SCAN_LEX_DECL_SIGIL,
    SCAN_LEX_DECL_REGEX,
    SCAN_LEX_DECL_HEREDOC,
    SCAN_LEX_DECL_EMBED,
    SCAN_LEX_DECL_FENCE,
    SCAN_LEX_DECL_DIRECTIVE,
    SCAN_LEX_DECL_OPERATOR,
    SCAN_LEX_DECL_SOFT_KEYWORD,
    SCAN_LEX_DECL_LITERAL_HOOK,
    SCAN_LEX_DECL_RECOVERY,
} scan_lex_decl_kind_t;

typedef struct {
    scan_lex_decl_kind_t kind;
    core_string_t        spelling;
    core_string_t        origin;
} scan_lex_declaration_t;

typedef enum : uint8_t {
    SCAN_LEX_ISSUE_UNUSED_OUTPUT,
    SCAN_LEX_ISSUE_SHADOWED_TERMINAL,
} scan_lex_issue_kind_t;

typedef struct {
    scan_lex_issue_kind_t kind;
    core_string_t         token;
    core_string_t         origin;
} scan_lex_issue_t;

typedef enum : uint8_t {
    SCAN_LEX_LAYOUT_NONE    = 0,
    SCAN_LEX_LAYOUT_NEWLINE = 1 << 0,
    SCAN_LEX_LAYOUT_INDENT  = 1 << 1,
} scan_lex_layout_flag;

typedef enum : uint8_t {
    SCAN_LEX_INDENT_ALLOW,
    SCAN_LEX_INDENT_REJECT,
} scan_lex_indent_policy_t;

typedef enum : uint8_t {
    SCAN_LEX_INDENT_IGNORE,
    SCAN_LEX_INDENT_APPLY,
} scan_lex_indent_line_policy_t;

typedef enum : uint8_t {
    SCAN_LEX_FALLBACK_CODEPOINT,
    SCAN_LEX_FALLBACK_REJECT,
} scan_lex_fallback_t;

typedef enum : uint8_t {
    SCAN_LEX_WHITESPACE_SKIP,
    SCAN_LEX_WHITESPACE_CODEPOINT,
} scan_lex_whitespace_t;

typedef enum : uint8_t {
    SCAN_LEX_NORMALIZE_NONE,
    SCAN_LEX_NORMALIZE_NFC,
    SCAN_LEX_NORMALIZE_NFKC,
} scan_lex_normalization_t;

typedef enum : uint8_t {
    SCAN_LEX_EMOJI_REJECT,
    SCAN_LEX_EMOJI_ALLOW,
    SCAN_LEX_EMOJI_DIAGNOSE,
} scan_lex_emoji_policy_t;

typedef struct {
    core_string_t              name;
    core_string_t              unicode_version;
    const core_string_t       *scripts;
    scan_lex_normalization_t   normalization;
    scan_lex_emoji_policy_t    emoji;
    bool                       diagnose_mixed_scripts;
    bool                       diagnose_confusables;
} scan_lex_identifier_profile_t;

typedef struct {
    core_string_t name;

    const scan_lex_comment_t *comments;
    const scan_lex_region_t  *regions;
    const scan_lex_string_t  *strings;
    const scan_lex_sigil_t   *sigils;
    const scan_lex_regex_t   *regexes;
    const scan_lex_heredoc_header_t *heredoc_headers;
    const scan_lex_heredoc_t *heredocs;
    const scan_lex_embed_t   *embeds;
    const scan_lex_fence_t   *fences;
    const scan_lex_literal_hook_t *literal_hooks;
    const scan_lex_recovery_t *recoveries;
    const scan_lex_declaration_t *declarations;
    const core_string_t      *operators;
    const core_string_t      *soft_keywords;

    const scan_lex_number_form_t *number_forms;

    uint8_t      layout;
    core_string_t tok_newline;
    bool          newline_each_line;
    core_string_t tok_terminator;
    core_string_t terminator_after;
    core_string_t tok_indent;
    core_string_t tok_dedent;
    core_string_t tok_eof;
    core_string_t tok_ident;

    uint8_t                       indent_tab_width;
    scan_lex_indent_policy_t      indent_tabs;
    scan_lex_indent_policy_t      indent_mixed;
    scan_lex_indent_policy_t      indent_alignment;
    scan_lex_indent_policy_t      indent_unmatched;
    scan_lex_indent_line_policy_t indent_blank_lines;
    scan_lex_indent_line_policy_t indent_comment_lines;

    core_string_t line_continue;
    core_string_t brackets;

    const scan_lex_directive_t *directives;

    core_string_t ident_start_extra;
    core_string_t ident_extra;
    core_string_t tok_ident_upper;
    scan_lex_identifier_profile_t identifier_profile;
    scan_lex_fallback_t fallback;
    scan_lex_whitespace_t whitespace;
} scan_lex_spec_t;

typedef struct scan_lex_t scan_lex_t;
typedef struct scan_lex_session_t scan_lex_session_t;

typedef struct {
    size_t comments;
    size_t strings;
    size_t sigils;
    size_t regexes;
    size_t heredocs;
    size_t embeds;
    size_t fences;
    size_t recoveries;
    size_t number_forms;
    size_t number_prefix_nodes;
    size_t number_prefix_edges;
    size_t number_suffix_nodes;
    size_t number_suffix_edges;
    size_t terminal_nodes;
    size_t terminal_edges;
    size_t opener_nodes;
    size_t opener_edges;
    size_t max_terminal_bytes;
    size_t resident_bytes;
    core_string_t unicode_version;
} scan_lex_info_t;

extern scan_lex_t *scan_lex_compile(const scan_lex_spec_t *spec,
                                  parse_grammar_t        *g);
extern void       scan_lex_free(scan_lex_t *lex);
extern scan_lex_session_t *scan_lex_session_new(const scan_lex_t *program);
extern void       *scan_lex_session_clone(const void *session);
extern bool        scan_lex_session_equal(const void *left,
                                          const void *right);
extern void        scan_lex_session_shift(void *session, size_t edit_start,
                                          size_t old_end,
                                          ptrdiff_t byte_delta);
extern void        scan_lex_session_free(void *session);

extern bool scan_lex_scan(parse_scanner_t *s);
extern bool scan_lex_finalize_regions(parse_scanner_t *s);
extern void scan_lex_reset_state(parse_scanner_t *s);
extern void scan_lex_compact_state(parse_scanner_t *s, size_t removed_bytes);

extern size_t scan_lex_operator_width(const scan_lex_t *lex, char first);
extern bool scan_lex_inspect(const scan_lex_t *lex, scan_lex_info_t *out);
extern size_t scan_lex_issue_count(const scan_lex_t *lex);
extern bool scan_lex_issue(const scan_lex_t *lex, size_t index,
                           scan_lex_issue_t *out);
extern core_string_t scan_lex_unicode_version(void);

extern bool                  scan_lex_register(const scan_lex_spec_t *spec);
extern const scan_lex_spec_t *scan_lex_for(core_string_t name);

extern void                  scan_lex_spec_free(const scan_lex_spec_t *spec);
extern const char           *scan_lex_build_error(void);
extern const char           *scan_lex_compile_error(void);
extern void                  scan_lex_compile_error_reset(void);

extern bool scan_lex_literal_hook_register_pure(core_string_t name,
                                                scan_lex_literal_hook_fn fn);
extern bool scan_lex_literal_hook_exists(core_string_t name);
extern bool scan_lex_literal_hook_call(core_string_t name,
                                       const scan_lex_literal_input_t *input,
                                       scan_lex_literal_result_t *result);
extern void scan_lex_literal_result_free(scan_lex_literal_result_t *result);

extern bool scan_grammar_set_lex(parse_grammar_t *g, const scan_lex_spec_t *spec);
extern const scan_lex_spec_t *scan_grammar_lex(parse_grammar_t *g);
extern scan_lex_t            *scan_grammar_lex_compiled(parse_grammar_t *g);
