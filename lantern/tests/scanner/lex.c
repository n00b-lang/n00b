#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "lib/buffer.h"
#include "parse/bnf.h"
#include "parse/grammar.h"
#include "parse/pwz.h"
#include "parse/tree_util.h"
#include "private/parse/grammar.h"
#include "scanner/lex.h"
#include "scanner/lex_conformance.h"
#include "scanner/token_stream.h"
#include "scanner/tokenizer.h"

#define S(x) CORE_STRING_STATIC(x)

#define HEX "0123456789abcdefABCDEF"

static scan_status_t source_status(parse_grammar_t *g, const char *source);
static parse_grammar_t *unicode_identifier_grammar(const char *version);
static atomic_size_t literal_hook_calls;

typedef struct {
  uint64_t magic;
  uint32_t format;
  uint32_t endian;
  uint32_t pointer_bytes;
  uint32_t size_bytes;
  uint64_t abi;
  uint64_t grammar;
  uint64_t payload_bytes;
  uint64_t root_offset;
  uint64_t relocation_count;
  uint64_t checksum;
} test_lex_image_header_t;

static scan_lex_literal_borrowed_result_t
test_literal_hook(const scan_lex_literal_input_t *input) {
  literal_hook_calls++;

  if (core_string_eq(input->modifier, S("set"))) {
    return (scan_lex_literal_borrowed_result_t){
        .accepted = true,
        .cacheable = true,
        .borrowed_value = S("set-value"),
        .borrowed_type = S("set[int]"),
    };
  }
  if (core_string_eq(input->spelling, S("\"bad\""))) {
    return (scan_lex_literal_borrowed_result_t){
        .cacheable = true,
        .borrowed_diagnostic = S("bad literal"),
        .diagnostic_start = 1,
        .diagnostic_end = 4,
    };
  }
  if (core_string_eq(input->spelling, S("\"wrong\""))) {
    return (scan_lex_literal_borrowed_result_t){
        .accepted = true,
        .cacheable = true,
        .borrowed_token = S("UNDECLARED"),
    };
  }
  return (scan_lex_literal_borrowed_result_t){
      .accepted = true,
      .cacheable = true,
      .borrowed_value = S("decoded"),
      .borrowed_token = S("SPECIAL"),
      .borrowed_type = S("duration"),
  };
}

static scan_lex_literal_borrowed_result_t
nested_literal_hook(const scan_lex_literal_input_t *input) {
  return (scan_lex_literal_borrowed_result_t){
      .accepted = input->spelling.data != nullptr,
      .borrowed_value = S("nested"),
  };
}

static scan_lex_literal_borrowed_result_t
reentrant_literal_hook(const scan_lex_literal_input_t *input) {
  scan_lex_literal_result_t nested = {0};
  bool accepted =
      scan_lex_literal_hook_call(S("nested-literal-hook"), input, &nested) &&
      nested.accepted;
  scan_lex_literal_result_free(&nested);
  return (scan_lex_literal_borrowed_result_t){
      .accepted = accepted,
      .borrowed_value = S("reentrant"),
  };
}

static const scan_lex_comment_t PY_COMMENTS[] = {
    {.kind = SCAN_LEX_LINE, .opener = S("#")},
    {},
};

static const core_string_t RECOVERY_SYNC[] = {
    S(";"),
    S("\n"),
    {},
};

static const scan_lex_recovery_t RECOVERIES[] = {
    {.token = S("INTEGER"),
     .replacement = S("ERROR"),
     .synchronize = RECOVERY_SYNC},
    {.token = S("STRING"),
     .replacement = S("ERROR"),
     .synchronize = RECOVERY_SYNC},
    {},
};

static const scan_lex_string_t RECOVERY_STRINGS[] = {
    {.opener = S("\""),
     .closer = S("\""),
     .escape = S("\\"),
     .simple_escapes = S("n"),
     .tok = S("STRING")},
    {},
};

static const scan_lex_spec_t RECOVERY_LEXER = {
    .name = S("recovery-test"),
    .strings = RECOVERY_STRINGS,
    .number = {.sep = S("_"), .tok_int = S("INTEGER")},
    .recoveries = RECOVERIES,
    .tok_ident = S("IDENTIFIER"),
};

static const scan_lex_spec_t REJECT_RECOVERY_LEXER = {
    .name = S("reject-recovery-test"),
    .recoveries = RECOVERIES,
    .fallback = SCAN_LEX_FALLBACK_REJECT,
};

static const core_string_t MULTI_RECOVERY_SYNC[] = {
    S(";;"),
    {},
};

static const scan_lex_recovery_t MULTI_RECOVERIES[] = {
    {.token = S("INTEGER"),
     .replacement = S("ERROR"),
     .synchronize = MULTI_RECOVERY_SYNC},
    {},
};

static const scan_lex_spec_t MULTI_RECOVERY_LEXER = {
    .name = S("multi-recovery-test"),
    .number = {.sep = S("_"), .tok_int = S("INTEGER")},
    .recoveries = MULTI_RECOVERIES,
    .tok_ident = S("IDENTIFIER"),
};

static const scan_lex_recovery_t CONSUMING_RECOVERIES[] = {
    {.token = S("INTEGER"),
     .replacement = S("ERROR"),
     .synchronize = RECOVERY_SYNC,
     .consume = true},
    {},
};

static const scan_lex_spec_t CONSUMING_RECOVERY_LEXER = {
    .name = S("consuming-recovery-test"),
    .number = {.sep = S("_"), .tok_int = S("INTEGER")},
    .recoveries = CONSUMING_RECOVERIES,
    .tok_ident = S("IDENTIFIER"),
};

static const scan_lex_string_t PY_STRINGS[] = {
    {.opener = S("\"\"\""),
     .closer = S("\"\"\""),
     .escape = S("\\"),
     .flags = SCAN_LEX_STR_MULTILINE,
     .tok = S("STRING")},
    {.opener = S("f\""),
     .closer = S("\""),
     .escape = S("\\"),
     .interp_open = S("{"),
     .interp_close = S("}"),
     .tok = S("FSTRING_START"),
     .tok_mid = S("FSTRING_MIDDLE"),
     .tok_end = S("FSTRING_END")},
    {.opener = S("\""),
     .closer = S("\""),
     .escape = S("\\"),
     .tok = S("STRING")},
    {},
};

static const scan_lex_radix_t PY_RADIXES[] = {
    {.prefix = S("0x"), .digits = S(HEX)},
    {.prefix = S("0b"), .digits = S("01")},
    {},
};

static const scan_lex_suffix_t PY_SUFFIXES[] = {
    {.suffix = S("j"), .tok = S("IMAGINARY")},
    {},
};

static const scan_lex_spec_t PYTHON = {
    .name = S("lex-test-python"),
    .comments = PY_COMMENTS,
    .strings = PY_STRINGS,
    .number = {.radixes = PY_RADIXES,
               .suffixes = PY_SUFFIXES,
               .sep = S("_"),
               .exponent = S("eE"),
               .tok_int = S("INTEGER"),
               .tok_float = S("FLOAT")},
    .layout = SCAN_LEX_LAYOUT_INDENT | SCAN_LEX_LAYOUT_NEWLINE,
    .tok_newline = S("NEWLINE"),
    .tok_indent = S("INDENT"),
    .tok_dedent = S("DEDENT"),
    .tok_eof = S("ENDMARKER"),
    .tok_ident = S("IDENTIFIER"),
    .line_continue = S("\\"),
    .brackets = S("()[]{}"),
};

static const scan_lex_comment_t JS_COMMENTS[] = {
    {.kind = SCAN_LEX_LINE, .opener = S("//")},
    {.kind = SCAN_LEX_BLOCK, .opener = S("/*"), .closer = S("*/")},
    {},
};

static const scan_lex_string_t JS_STRINGS[] = {
    {.opener = S("`"),
     .closer = S("`"),
     .escape = S("\\"),
     .flags = SCAN_LEX_STR_MULTILINE,
     .interp_open = S("${"),
     .interp_close = S("}"),
     .tok = S("TEMPLATE_HEAD"),
     .tok_mid = S("TEMPLATE_MIDDLE"),
     .tok_end = S("TEMPLATE_TAIL")},
    {.opener = S("\""),
     .closer = S("\""),
     .escape = S("\\"),
     .tok = S("STRING")},
    {},
};

static const core_string_t JS_REGEX_NOT_AFTER[] = {
    S("IDENTIFIER"), S("INTEGER"), S("FLOAT"), S("STRING"), S("TEMPLATE_TAIL"),
    S("REGEX_LIT"),  S(")"),       S("]"),     S("}"),      S("++"),
    S("--"),         S("this"),    S("super"), S("true"),   S("false"),
    S("null"),       {},
};

static const scan_lex_regex_t JS_REGEXES[] = {
    {.opener = S("/"),
     .closer = S("/"),
     .suffix_chars = S("gimsuyd"),
     .tok = S("REGEX_LIT"),
     .not_after = JS_REGEX_NOT_AFTER},
    {},
};

static const scan_lex_spec_t JS = {
    .name = S("lex-test-js"),
    .comments = JS_COMMENTS,
    .strings = JS_STRINGS,
    .regexes = JS_REGEXES,
    .number = {.radixes = PY_RADIXES,
               .exponent = S("eE"),
               .tok_int = S("INTEGER"),
               .tok_float = S("FLOAT")},
    .tok_ident = S("IDENTIFIER"),
    .brackets = S("()[]{}"),
};

static const scan_lex_heredoc_t RB_HEREDOCS[] = {
    {.opener = S("<<~"),
     .tok = S("HEREDOC"),
     .header = {.quotes = SCAN_LEX_HEREDOC_UNQUOTED |
                          SCAN_LEX_HEREDOC_SINGLE_QUOTED |
                          SCAN_LEX_HEREDOC_DOUBLE_QUOTED,
                .trailing_text = true},
     .terminator_indent = true,
     .body_indent_strip = true},
    {},
};

static const scan_lex_sigil_t RB_SIGILS[] = {
    {.prefix = S("@@"), .tok = S("CVAR")},
    {.prefix = S("@"), .tok = S("IVAR")},
    {.prefix = S("$"), .tok = S("GVAR")},
    {},
};

static const scan_lex_spec_t RUBY = {
    .name = S("lex-test-ruby"),
    .heredocs = RB_HEREDOCS,
    .sigils = RB_SIGILS,
    .number = {.tok_int = S("INTEGER")},
    .tok_ident = S("IDENTIFIER"),
};

static const scan_lex_embed_t NESTED_EMBEDS[] = {
    {.open = S("["),
     .close = S("]"),
     .open_tok = S("INNER_OPEN"),
     .close_tok = S("INNER_CLOSE"),
     .lexer = S("json"),
     .tokens = true},
    {},
};

static const scan_lex_spec_t NESTED = {
    .name = S("nested-embed-test"),
    .embeds = NESTED_EMBEDS,
    .tok_ident = S("MIDDLE_ID"),
};

static const scan_lex_embed_t CYCLE_A_EMBEDS[] = {
    {.open = S("<a>"),
     .close = S("</a>"),
     .open_tok = S("A_OPEN"),
     .close_tok = S("A_CLOSE"),
     .lexer = S("embed-cycle-b"),
     .tokens = true},
    {},
};

static const scan_lex_embed_t CYCLE_B_EMBEDS[] = {
    {.open = S("<b>"),
     .close = S("</b>"),
     .open_tok = S("B_OPEN"),
     .close_tok = S("B_CLOSE"),
     .lexer = S("embed-cycle-a"),
     .tokens = true},
    {},
};

static const scan_lex_spec_t CYCLE_A = {
    .name = S("embed-cycle-a"),
    .embeds = CYCLE_A_EMBEDS,
};

static const scan_lex_spec_t CYCLE_B = {
    .name = S("embed-cycle-b"),
    .embeds = CYCLE_B_EMBEDS,
};

static void register_output(parse_grammar_t *g, core_string_t name) {
  if (name.data && name.u8_bytes && !parse_terminal_id(g, name)) {
    parse_register_terminal(g, name);
  }
}

static void register_spec_outputs(parse_grammar_t *g,
                                  const scan_lex_spec_t *spec) {
  register_output(g, spec->tok_newline);
  register_output(g, spec->tok_terminator);
  register_output(g, spec->tok_indent);
  register_output(g, spec->tok_dedent);
  register_output(g, spec->tok_eof);
  register_output(g, spec->tok_ident);
  register_output(g, spec->tok_ident_upper);
  register_output(g, spec->number.tok_int);
  register_output(g, spec->number.tok_float);

  for (const scan_lex_comment_t *item = spec->comments;
       item && item->opener.data; item++) {
    register_output(g, item->tok);
  }
  for (const scan_lex_string_t *item = spec->strings; item && item->opener.data;
       item++) {
    register_output(g, item->tok);
    register_output(g, item->tok_mid);
    register_output(g, item->tok_end);
  }
  for (const scan_lex_number_form_t *item = spec->number_forms;
       item && item->digits.data; item++) {
    register_output(g, item->tok_int);
    register_output(g, item->tok_float);
    for (const scan_lex_suffix_t *suffix = item->suffixes;
         suffix && suffix->suffix.data; suffix++) {
      register_output(g, suffix->tok);
    }
  }
  for (const scan_lex_radix_t *item = spec->number.radixes;
       item && item->prefix.data; item++) {
    register_output(g, item->tok);
  }
  for (const scan_lex_suffix_t *item = spec->number.suffixes;
       item && item->suffix.data; item++) {
    register_output(g, item->tok);
  }
  for (const scan_lex_sigil_t *item = spec->sigils; item && item->prefix.data;
       item++) {
    register_output(g, item->tok);
  }
  for (const scan_lex_regex_t *item = spec->regexes; item && item->opener.data;
       item++) {
    register_output(g, item->tok);
  }
  for (const scan_lex_heredoc_t *item = spec->heredocs;
       item && item->opener.data; item++) {
    register_output(g, item->tok);
    register_output(g, item->interpolated_tok);
  }
  for (const scan_lex_embed_t *item = spec->embeds; item && item->open.data;
       item++) {
    register_output(g, item->tok);
    register_output(g, item->open_tok);
    register_output(g, item->close_tok);
  }
  for (const scan_lex_fence_t *item = spec->fences;
       item && item->outer_open.data; item++) {
    register_output(g, item->tok);
  }
  for (const scan_lex_directive_t *item = spec->directives;
       item && item->open.data; item++) {
    register_output(g, item->tok_eol);
    register_output(g, item->tok_other);
  }
  for (const scan_lex_literal_hook_t *item = spec->literal_hooks;
       item && item->token.data; item++) {
    register_output(g, item->token);
    for (const core_string_t *output = item->outputs; output && output->data;
         output++) {
      register_output(g, *output);
    }
  }
  for (const scan_lex_recovery_t *item = spec->recoveries;
       item && item->token.data; item++) {
    register_output(g, item->token);
    register_output(g, item->replacement);
  }
}

static parse_grammar_t *mk(const scan_lex_spec_t *spec,
                           const char *const *terms) {
  parse_grammar_t *g = parse_grammar_new();

  for (size_t i = 0; terms && terms[i]; i++) {
    parse_register_terminal(g, core_string_from_cstr(terms[i]));
  }
  register_spec_outputs(g, spec);

  scan_lex_register(spec);
  scan_grammar_set_tokenizer(g, spec->name);

  return g;
}

static parse_grammar_t *bare_grammar(const char *const *terms) {
  parse_grammar_t *g = parse_grammar_new();

  for (size_t i = 0; terms && terms[i]; i++) {
    parse_register_terminal(g, core_string_from_cstr(terms[i]));
  }
  return g;
}

static char *render(parse_grammar_t *g, const char *src) {
  static char out[2048];
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;

  out[0] = '\0';

  parse_token_stream_t *ts = scan_tokenize(g, src, strlen(src), &buf, &sc);

  if (!ts) {
    return out;
  }

  size_t at = 0;

  scan_stream_foreach(ts, tok) {
    const char *nm = nullptr;

    for (size_t i = 0; i < core_list_len(g->named_terms); i++) {
      if (g->named_terms.data[i].id == tok->tid) {
        nm = g->named_terms.data[i].value.data;

        break;
      }
    }

    if (nm) {
      at += (size_t)snprintf(out + at, sizeof(out) - at, "%s%s", at ? " " : "",
                             nm);
    } else if (tok->tid > 0 && tok->tid < 128) {
      at += (size_t)snprintf(out + at, sizeof(out) - at, "%s%c", at ? " " : "",
                             (char)tok->tid);
    } else {
      at += (size_t)snprintf(out + at, sizeof(out) - at, "%s?%d", at ? " " : "",
                             tok->tid);
    }
  }

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  return out;
}

static void kinds(parse_grammar_t *g, const char *what, const char *src,
                  const char *want) {
  char *got = render(g, src);

  if (strcmp(got, want)) {
    fails++;
    printf("FAIL  %s\n   want: %s\n    got: %s\n", what, want, got);
  }
}

static const char *const PY_TERMS[] = {
    "NEWLINE",        "INDENT",      "DEDENT", "ENDMARKER", "IDENTIFIER",
    "INTEGER",        "FLOAT",       "STRING", "IMAGINARY", "FSTRING_START",
    "FSTRING_MIDDLE", "FSTRING_END", "def",    "if",        "return",
    nullptr,
};

static parse_grammar_t *indent_policy_grammar(const char *policies) {
  core_buffer_t *text = core_buffer_empty();

  core_buffer_printf(
      text,
      "@lex-comment(\"#\")\n"
      "@lex-layout(indent(\"INDENT\", \"DEDENT\"), "
      "newline(\"NEWLINE\"), eof(\"ENDMARKER\"), continue(\"\\\\\"), "
      "brackets(\"()[]{}\")%s%s)\n"
      "@lex-ident(as(\"IDENTIFIER\"))\n"
      "<file> ::= %%IDENTIFIER %%NEWLINE %%INDENT %%DEDENT %%ENDMARKER\n",
      policies && policies[0] ? ", " : "", policies ? policies : "");

  parse_grammar_t *g = parse_grammar_new();
  core_string_t source = core_string_borrow(text->data, text->byte_len);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, source);

  if (!parse_bnf_load(source, CORE_STRING_STATIC("file"), g)) {
    fprintf(stderr, "indent policy grammar: %s\n", parse_bnf_error());
    parse_grammar_free(g);
    g = nullptr;
  }

  core_buffer_free(text);
  return g;
}

static void test_indentation_becomes_tokens(void) {
  parse_grammar_t *g = mk(&PYTHON, PY_TERMS);

  kinds(g, "an indented block opens and closes", "def f():\n    return 1\n",
        "def IDENTIFIER ( ) : NEWLINE INDENT return INTEGER NEWLINE "
        "DEDENT ENDMARKER");

  kinds(g, "a dedent lands before the token that caused it",
        "if x:\n  a\n  b\nc\n",
        "if IDENTIFIER : NEWLINE INDENT IDENTIFIER NEWLINE IDENTIFIER "
        "NEWLINE DEDENT IDENTIFIER NEWLINE ENDMARKER");

  kinds(g, "a newline inside brackets is not significant", "a = (1,\n     2)\n",
        "IDENTIFIER = ( INTEGER , INTEGER ) NEWLINE ENDMARKER");

  kinds(g, "a blank line does not dedent", "if x:\n  a\n\n  b\n",
        "if IDENTIFIER : NEWLINE INDENT IDENTIFIER NEWLINE IDENTIFIER "
        "NEWLINE DEDENT ENDMARKER");

  kinds(g, "a comment line does not dedent", "if x:\n  a\n# note\n  b\n",
        "if IDENTIFIER : NEWLINE INDENT IDENTIFIER NEWLINE IDENTIFIER "
        "NEWLINE DEDENT ENDMARKER");

  kinds(g, "a continued line is one logical line", "a = 1 + \\\n    2\n",
        "IDENTIFIER = INTEGER + INTEGER NEWLINE ENDMARKER");

  kinds(g, "nested blocks close in one step", "if a:\n  if b:\n    c\nd\n",
        "if IDENTIFIER : NEWLINE INDENT if IDENTIFIER : NEWLINE INDENT "
        "IDENTIFIER NEWLINE DEDENT DEDENT IDENTIFIER NEWLINE ENDMARKER");

  parse_grammar_free(g);

  parse_grammar_t *layout =
      indent_policy_grammar("unmatched_dedent(\"allow\")");
  static const char *const ALIGNED = "a\n    b\n        c\nx\n";
  parse_token_stream_t *ts =
      scan_tokenize_owned(layout, ALIGNED, strlen(ALIGNED));
  parse_token_info_t *dedents[4] = {0};
  size_t dedent_count = 0;
  parse_token_info_t *tok;
  while ((tok = scan_stream_next(ts))) {
    if (tok->tid == parse_terminal_id(layout, S("DEDENT")) &&
        dedent_count < 4) {
      dedents[dedent_count++] = tok;
    }
  }
  check("each dedent records its resulting depth and column",
        dedent_count == 2 && dedents[0]->has_layout_info &&
            dedents[0]->layout_depth == 1 && dedents[0]->layout_column == 4 &&
            dedents[1]->layout_depth == 0 && dedents[1]->layout_column == 0);
  scan_token_stream_free(ts);

  static const char *const MISALIGNED = "a\n    b\n        c\n  x\n";
  ts = scan_tokenize_owned(layout, MISALIGNED, strlen(MISALIGNED));
  dedent_count = 0;
  while ((tok = scan_stream_next(ts))) {
    if (tok->tid == parse_terminal_id(layout, S("DEDENT")) &&
        dedent_count < 4) {
      dedents[dedent_count++] = tok;
    }
  }
  check("only the final dedent carries the unmatched-target marker",
        dedent_count == 2 && !dedents[0]->layout_mismatch &&
            dedents[1]->layout_mismatch && dedents[1]->layout_depth == 0 &&
            dedents[1]->layout_column == 0);
  scan_token_stream_free(ts);
  parse_grammar_free(layout);
}

static void test_indentation_policies(void) {
  parse_grammar_t *width = indent_policy_grammar(
      "tab_width(\"4\"), tabs(\"allow\"), mixed_indent(\"allow\"), "
      "alignment(\"allow\"), unmatched_dedent(\"allow\"), "
      "blank_lines(\"ignore\"), comment_lines(\"ignore\")");

  check("all indentation policies are accepted declaratively",
        width != nullptr);

  if (width) {
    kinds(width, "the declared tab width determines indentation columns",
          "a\n    b\n\tc\n",
          "IDENTIFIER NEWLINE INDENT IDENTIFIER NEWLINE IDENTIFIER "
          "NEWLINE DEDENT ENDMARKER");
    parse_grammar_free(width);
  }

  parse_grammar_t *tabs = indent_policy_grammar("tabs(\"reject\")");
  parse_token_stream_t *ts = scan_tokenize_owned(tabs, "a\n\tb\n", 5);

  while (scan_stream_next(ts)) {
  }

  scan_diagnostic_t diagnostic = {0};
  check("tabs can be rejected with a one-byte diagnostic span",
        scan_diagnostic(ts->scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_INVALID &&
            diagnostic.start_byte == 2 && diagnostic.end_byte == 3);
  scan_token_stream_free(ts);
  parse_grammar_free(tabs);

  tabs = indent_policy_grammar("tabs(\"reject\")");
  ts = scan_tokenize_owned(tabs, "a\tb", 3);
  while (scan_stream_next(ts)) {
  }
  diagnostic = (scan_diagnostic_t){0};
  check("tabs are rejected between tokens, not only in indentation",
        scan_diagnostic(ts->scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_INVALID &&
            diagnostic.start_byte == 1 && diagnostic.end_byte == 2);
  scan_token_stream_free(ts);
  parse_grammar_free(tabs);

  parse_grammar_t *mixed = indent_policy_grammar("mixed_indent(\"reject\")");
  ts = scan_tokenize_owned(mixed, "a\n \tb\n", 6);

  while (scan_stream_next(ts)) {
  }

  diagnostic = (scan_diagnostic_t){0};
  check("mixed indentation can be rejected at the indentation prefix",
        scan_diagnostic(ts->scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_INVALID &&
            diagnostic.start_byte == 2 && diagnostic.end_byte == 4);
  scan_token_stream_free(ts);
  parse_grammar_free(mixed);

  parse_grammar_t *unmatched =
      indent_policy_grammar("unmatched_dedent(\"reject\")");
  ts = scan_tokenize_owned(unmatched, "a\n  b\n c\n", 10);

  while (scan_stream_next(ts)) {
  }

  diagnostic = (scan_diagnostic_t){0};
  check("an unmatched dedent identifies only its indentation prefix",
        scan_diagnostic(ts->scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_INVALID &&
            diagnostic.start_byte == 6 && diagnostic.end_byte == 7);
  scan_token_stream_free(ts);
  parse_grammar_free(unmatched);

  parse_grammar_t *alignment = indent_policy_grammar("alignment(\"reject\")");
  ts = scan_tokenize_owned(alignment, "a = (\n  b)\n", 12);

  while (scan_stream_next(ts)) {
  }

  diagnostic = (scan_diagnostic_t){0};
  check("alignment policy composes with bracket suppression",
        scan_diagnostic(ts->scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_INVALID &&
            diagnostic.start_byte == 6 && diagnostic.end_byte == 8);
  scan_token_stream_free(ts);

  ts = scan_tokenize_owned(alignment, "a \\\n  b\n", 8);

  while (scan_stream_next(ts)) {
  }

  diagnostic = (scan_diagnostic_t){0};
  check("alignment policy composes with explicit line continuation",
        scan_diagnostic(ts->scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_INVALID &&
            diagnostic.start_byte == 4 && diagnostic.end_byte == 6);
  scan_token_stream_free(ts);
  parse_grammar_free(alignment);

  parse_grammar_t *physical =
      indent_policy_grammar("blank_lines(\"apply\"), comment_lines(\"apply\")");

  kinds(physical, "blank lines can participate in indentation",
        "a\n  b\n\n  c\n",
        "IDENTIFIER NEWLINE INDENT IDENTIFIER NEWLINE DEDENT INDENT "
        "IDENTIFIER NEWLINE DEDENT ENDMARKER");
  kinds(physical, "comment-only lines can participate in indentation",
        "a\n  b\n# note\n  c\n",
        "IDENTIFIER NEWLINE INDENT IDENTIFIER NEWLINE DEDENT INDENT "
        "IDENTIFIER NEWLINE DEDENT ENDMARKER");
  parse_grammar_free(physical);

  parse_grammar_t *bad = indent_policy_grammar("tab_width(\"0\")");

  check("invalid indentation policy values fail lexer construction",
        bad && scan_grammar_lex(bad) == nullptr && scan_lex_build_error() &&
            strstr(scan_lex_build_error(), "tab_width") != nullptr);
  parse_grammar_free(bad);
}

static parse_grammar_t *layout_matching_grammar(bool colon_block) {
  static const char *const OUTSIDE =
      "@lex-layout(indent(\"INDENT\", \"DEDENT\"), "
      "newline(\"NEWLINE\"), eof(\"END\"))\n"
      "@lex-ident(as(\"IDENTIFIER\"))\n"
      "<file> ::= %IDENTIFIER %NEWLINE %IDENTIFIER %NEWLINE %END\n";
  static const char *const BLOCK =
      "@lex-layout(indent(\"INDENT\", \"DEDENT\"), "
      "newline(\"NEWLINE\"), eof(\"END\"))\n"
      "@lex-ident(as(\"IDENTIFIER\"))\n"
      "<file> ::= %IDENTIFIER %\":\" %NEWLINE %INDENT %IDENTIFIER "
      "%NEWLINE %DEDENT %IDENTIFIER %NEWLINE %END\n";
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_from_cstr(colon_block ? BLOCK : OUTSIDE);
  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = parse_bnf_load(text, S("file"), g);
  if (!loaded) {
    fprintf(stderr, "layout grammar load failed: %s\n", parse_bnf_error());
  }
  core_free(text.data);
  if (!loaded) {
    parse_grammar_free(g);
    return nullptr;
  }
  parse_register_terminal(g, S("INDENT"));
  parse_register_terminal(g, S("DEDENT"));
  return g;
}

static void test_layout_tokens_ignore_only_when_unmatched(void) {
  parse_grammar_t *g = layout_matching_grammar(false);
  static const char *const OUTSIDE = "a\n  b\n";
  parse_token_stream_t *ts = scan_tokenize_owned(g, OUTSIDE, strlen(OUTSIDE));
  parse_pwz_parser_t *parser = parse_pwz_new_for_start(
      g, parse_nonterm_id(parse_find_nonterm(g, S("file"))));
  bool parsed = parse_pwz_parse(parser, ts);
  check("unmatched ordinary indentation tokens do not block parsing", parsed);
  parse_pwz_free(parser);
  scan_token_stream_free(ts);
  parse_grammar_free(g);

  g = layout_matching_grammar(true);
  static const char *const BLOCK = "a:\n  b\nc\n";
  ts = scan_tokenize_owned(g, BLOCK, strlen(BLOCK));
  parse_token_info_t *tok;
  parse_token_info_t *block_indent = nullptr;
  parse_token_info_t *block_dedent = nullptr;
  while ((tok = scan_stream_next(ts))) {
    if (tok->tid == parse_terminal_id(g, S("INDENT"))) {
      block_indent = tok;
    } else if (tok->tid == parse_terminal_id(g, S("DEDENT"))) {
      block_dedent = tok;
    }
  }
  parser = parse_pwz_new_for_start(
      g, parse_nonterm_id(parse_find_nonterm(g, S("file"))));
  check("a colon-newline block keeps both layout tokens significant",
        block_indent && block_dedent && !block_indent->ignore_if_no_match &&
            !block_dedent->ignore_if_no_match && parse_pwz_parse(parser, ts));
  parse_pwz_free(parser);
  scan_token_stream_free(ts);
  parse_grammar_free(g);

  g = layout_matching_grammar(true);
  static const char *const BLOCK_U2028 = "a:\n  b\xE2\x80\xA8"
                                         "c\n";
  ts = scan_tokenize_owned(g, BLOCK_U2028, strlen(BLOCK_U2028));
  kinds(g, "U+2028 layout token sequence", BLOCK_U2028,
        "IDENTIFIER : NEWLINE INDENT IDENTIFIER NEWLINE DEDENT IDENTIFIER "
        "NEWLINE END");
  parser = parse_pwz_new_for_start(
      g, parse_nonterm_id(parse_find_nonterm(g, S("file"))));
  check("U+2028 terminates an indentation block",
        ts && parse_pwz_parse(parser, ts));
  parse_pwz_free(parser);
  scan_token_stream_free(ts);
  parse_grammar_free(g);
}

static void test_incremental_indentation_waits_for_content(void) {
  parse_grammar_t *g = indent_policy_grammar("");
  parse_token_stream_t *ts = scan_tokenize_incremental_owned(g);

  check("an incremental indentation prefix can be appended",
        scan_stream_append(ts, "a\n  ", 4));
  check("tokens before an unfinished indentation prefix remain available",
        scan_stream_next(ts) != nullptr && scan_stream_next(ts) != nullptr);
  check("indentation waits without consuming its temporary boundary",
        scan_stream_next(ts) == nullptr &&
            scan_status(ts->scanner) == SCAN_STATUS_INCOMPLETE &&
            ts->scanner->cursor == 2);
  check("the completed indentation still emits its indent token",
        scan_stream_append(ts, "b\n", 2) && scan_stream_next(ts) != nullptr);

  parse_token_info_t *b = scan_stream_next(ts);
  check("content follows the resumed indent token",
        b && b->tid == parse_terminal_id(g, S("IDENTIFIER")));

  scan_stream_finish(ts);
  while (scan_stream_next(ts)) {
  }
  check("the resumed indentation stream completes",
        scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);

  scan_token_stream_free(ts);

  ts = scan_tokenize_incremental_owned(g);
  check("a line-continuation prefix can end an incremental chunk",
        scan_stream_append(ts, "a \\", 3) && scan_stream_next(ts) != nullptr &&
            scan_stream_next(ts) == nullptr &&
            scan_status(ts->scanner) == SCAN_STATUS_INCOMPLETE &&
            ts->scanner->cursor == 2);
  check("continued-line alignment resumes without indentation tokens",
        scan_stream_append(ts, "\n  b\n", 5) &&
            scan_stream_next(ts) != nullptr);
  scan_stream_finish(ts);
  while (scan_stream_next(ts)) {
  }
  check("a split line continuation completes",
        scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);

  scan_token_stream_free(ts);
  parse_grammar_free(g);
}

static parse_grammar_t *embedded_grammar(const char *lexer, bool tokens) {
  core_buffer_t *text = core_buffer_empty();

  core_buffer_printf(
      text,
      "@lex-embed(\"{{\", \"}}\", lexer(\"%s\"), %s)\n"
      "@lex-ident(as(\"OUTER_ID\"))\n"
      "<file> ::= %%OUTER_ID | %%EMBED | %%EMBED_OPEN | %%EMBED_CLOSE | "
      "%%INNER_OPEN | %%INNER_CLOSE | %%MIDDLE_ID | %%IDENTIFIER | "
      "%%INT_LIT | %%FLOAT_LIT | %%STRING | %%A_OPEN | %%A_CLOSE | "
      "%%B_OPEN | %%B_CLOSE\n",
      lexer,
      tokens ? "tokens, open_as(\"EMBED_OPEN\"), close_as(\"EMBED_CLOSE\")"
             : "as(\"EMBED\")");

  parse_grammar_t *g = parse_grammar_new();
  core_string_t source = core_string_borrow(text->data, text->byte_len);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, source);

  if (!parse_bnf_load(source, CORE_STRING_STATIC("file"), g)) {
    parse_grammar_free(g);
    g = nullptr;
  }

  core_buffer_free(text);
  return g;
}

static void embed_error(const char *label, const char *declaration,
                        const char *expected) {
  core_buffer_t *text = core_buffer_from_cstr(declaration);
  core_buffer_puts(text, "\n<file> ::= %EMBED | %EMBED_OPEN | %EMBED_CLOSE\n");
  core_string_t source = core_string_borrow(text->data, text->byte_len);
  parse_grammar_t *g = parse_grammar_new();

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, source);
  bool loaded = parse_bnf_load(source, CORE_STRING_STATIC("file"), g);
  scan_lex_t *compiled = loaded ? scan_grammar_lex_compiled(g) : nullptr;
  const char *error = scan_lex_build_error();

  check(label, loaded && !compiled && error && strstr(error, expected));
  parse_grammar_free(g);
  core_buffer_free(text);
}

static void register_deep_embed_chain(void) {
  static bool initialized;
  static scan_lex_spec_t specs[80];
  static scan_lex_embed_t embeds[80][2];
  static char names[80][32];

  if (initialized) {
    return;
  }

  initialized = true;
  for (size_t i = 0; i < 80; i++) {
    int len = snprintf(names[i], sizeof(names[i]), "embed-depth-%zu", i);
    specs[i].name = core_string_borrow(names[i], (size_t)len);

    if (i + 1 < 80) {
      embeds[i][0] = (scan_lex_embed_t){
          .open = S("<"),
          .close = S(">"),
          .open_tok = S("EMBED_OPEN"),
          .close_tok = S("EMBED_CLOSE"),
          .tokens = true,
      };
      int next_len = snprintf(names[i + 1], sizeof(names[i + 1]),
                              "embed-depth-%zu", i + 1);
      embeds[i][0].lexer = core_string_borrow(names[i + 1], (size_t)next_len);
      specs[i].embeds = embeds[i];
    }
  }

  for (size_t i = 0; i < 80; i++) {
    scan_lex_register(&specs[i]);
  }
}

static void test_embedded_lexer_switching(void) {
  scan_lex_register(&NESTED);

  parse_grammar_t *tokens = embedded_grammar("json", true);
  scan_lex_info_t info = {0};

  check("compiled lexer inspection includes embedded recipes",
        scan_lex_inspect(scan_grammar_lex_compiled(tokens), &info) &&
            info.embeds == 1);

  kinds(tokens, "an embedded lexer emits its own token sequence",
        "before {{1 \"x\"}} after",
        "OUTER_ID EMBED_OPEN INT_LIT STRING_LIT EMBED_CLOSE OUTER_ID");

  parse_token_stream_t *ts =
      scan_tokenize_owned(tokens, "before {{1 \"x\"}} after", 22);
  parse_token_info_t *tok = nullptr;
  size_t count = 0;
  bool spans = true;

  while ((tok = scan_stream_next(ts))) {
    if (count == 1) {
      spans = tok->start_byte == 7 && tok->end_byte == 9;
    } else if (count == 2) {
      spans = spans && tok->start_byte == 9 && tok->end_byte == 10;
    } else if (count == 4) {
      spans = spans && tok->start_byte == 14 && tok->end_byte == 16;
    }
    count++;
  }
  check("embedded tokens and delimiters retain absolute source spans",
        spans && count == 6);
  scan_token_stream_free(ts);

  ts = scan_tokenize_incremental_owned(tokens);
  count = 0;
  const char *source = "before {{1 \"x\"}} after";

  for (size_t i = 0; source[i]; i++) {
    scan_stream_append(ts, source + i, 1);
    while (scan_stream_next(ts)) {
      count++;
    }
  }
  scan_stream_finish(ts);
  while (scan_stream_next(ts)) {
    count++;
  }
  check("embedding resumes across every delimiter and body boundary",
        count == 6 && scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);
  scan_token_stream_free(ts);

  ts = scan_tokenize_owned(tokens, "x {{\"open}}", 12);
  while (scan_stream_next(ts)) {
  }
  scan_diagnostic_t diagnostic = {0};
  check("inner diagnostics retain the embedded lexer identity",
        scan_diagnostic(ts->scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_INCOMPLETE &&
            core_string_eq(diagnostic.lexer, S("json")) &&
            diagnostic.start_byte >= 4);
  scan_token_stream_free(ts);
  parse_grammar_free(tokens);

  parse_grammar_t *single = embedded_grammar("json", false);
  ts = scan_tokenize_owned(single, "x {{1}} y", 9);
  parse_token_info_t *outer = scan_stream_next(ts);
  parse_token_info_t *block = scan_stream_next(ts);

  check("single-token embedding includes delimiters and records its body",
        outer && block && block->tid == parse_terminal_id(single, S("EMBED")) &&
            block->start_byte == 2 && block->end_byte == 7 &&
            block->has_content_span && block->content_start_byte == 4 &&
            block->content_end_byte == 5);
  scan_token_stream_free(ts);

  ts = scan_tokenize_owned(single, "x {{\"open}} y", 13);
  while (scan_stream_next(ts)) {
  }
  diagnostic = (scan_diagnostic_t){0};
  check("single-token embedding still validates with its named lexer",
        scan_diagnostic(ts->scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_INVALID &&
            core_string_eq(diagnostic.lexer, S("json")) &&
            diagnostic.start_byte == 4);
  scan_token_stream_free(ts);
  parse_grammar_free(single);

  parse_grammar_t *nested = embedded_grammar("nested-embed-test", true);

  kinds(nested, "embedded lexers can nest without mutable shared state",
        "a {{middle [1] tail}} z",
        "OUTER_ID EMBED_OPEN MIDDLE_ID INNER_OPEN INT_LIT INNER_CLOSE "
        "MIDDLE_ID EMBED_CLOSE OUTER_ID");
  parse_grammar_free(nested);

  scan_lex_register(&CYCLE_A);
  scan_lex_register(&CYCLE_B);
  parse_grammar_t *cycle = embedded_grammar("embed-cycle-a", true);
  scan_lex_t *compiled = scan_grammar_lex_compiled(cycle);

  check("embedded lexer cycles are rejected during construction",
        !compiled && scan_lex_compile_error() &&
            strstr(scan_lex_compile_error(), "cycle"));
  parse_grammar_free(cycle);

  register_deep_embed_chain();
  parse_grammar_t *deep = embedded_grammar("embed-depth-0", true);
  compiled = scan_grammar_lex_compiled(deep);
  check("embedded lexer nesting is not limited to 64 levels",
        compiled != nullptr);
  core_string_t deep_image =
      compiled ? scan_lex_image_encode(compiled) : (core_string_t){0};
  parse_grammar_t *deep_loaded = embedded_grammar("embed-depth-0", true);
  check("deep embedded lexer graphs survive image loading",
        deep_image.data && deep_loaded &&
            scan_grammar_set_lex_image(deep_loaded, deep_image));
  core_free(deep_image.data);
  parse_grammar_free(deep_loaded);
  parse_grammar_free(deep);

  embed_error("delegated embeds name unknown lexers",
              "@lex-embed(\"{{\", \"}}\", lexer(\"missing\"), "
              "as(\"EMBED\"))",
              "unknown lexer");
  embed_error("token-sequence embeds require explicit delimiter tokens",
              "@lex-embed(\"{{\", \"}}\", lexer(\"json\"), tokens)", "open_as");

  static const char *const LEGACY =
      "@lex-embed(\"{{\", \"}}\", as(\"OUTSIDE\"), "
      "open_as(\"OPEN\"), close_as(\"CLOSE\"))\n"
      "@lex-ident(as(\"IDENTIFIER\"))\n"
      "<file> ::= %OUTSIDE | %OPEN | %CLOSE | %IDENTIFIER\n";
  core_string_t legacy_source = core_string_from_cstr(LEGACY);
  parse_grammar_t *legacy = parse_grammar_new();

  parse_grammar_set_error_recovery(legacy, false);
  parse_bnf_declare_terminals(legacy, legacy_source);
  check("legacy embedded regions load alongside delegated embeds",
        parse_bnf_load(legacy_source, S("file"), legacy));

  ts = scan_tokenize_incremental_owned(legacy);
  check("a legacy opener prefix waits at an incremental boundary",
        scan_stream_append(ts, "a {", 3) && scan_stream_next(ts) != nullptr &&
            scan_stream_next(ts) == nullptr &&
            scan_status(ts->scanner) == SCAN_STATUS_INCOMPLETE);
  check("a legacy opener resumes without losing prefix bytes",
        scan_stream_append(ts, "{x}}", 4));
  scan_stream_finish(ts);
  count = 1;
  while (scan_stream_next(ts)) {
    count++;
  }
  check("the resumed legacy region emits its delimiters and body",
        count == 4 && scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);
  scan_token_stream_free(ts);
  parse_grammar_free(legacy);
  core_free(legacy_source.data);
}

static void test_interpolation_pushes_a_mode(void) {
  parse_grammar_t *g = mk(&PYTHON, PY_TERMS);

  kinds(g, "an f-string with one hole is head and end", "f\"a{b}c\"",
        "FSTRING_START IDENTIFIER FSTRING_END NEWLINE ENDMARKER");

  kinds(g, "two holes put a middle between them", "f\"{x}{y}\"",
        "FSTRING_START IDENTIFIER FSTRING_MIDDLE IDENTIFIER FSTRING_END "
        "NEWLINE ENDMARKER");

  kinds(g, "an f-string with no hole is one token", "f\"plain\"",
        "FSTRING_START NEWLINE ENDMARKER");

  kinds(g, "a triple-quoted string spans lines", "\"\"\"a\nb\"\"\"\n",
        "STRING NEWLINE ENDMARKER");

  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  const char *multiline = "\"\"\"a\nb\"\"\"";
  parse_token_stream_t *ts =
      scan_tokenize(g, multiline, strlen(multiline), &buf, &sc);
  parse_token_info_t *string = ts ? scan_stream_next(ts) : nullptr;

  check("a multiline token records both endpoint lines",
        string && string->line == 1 && string->column == 1 &&
            string->endline == 2 && string->endcol == 5);

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  parse_grammar_free(g);
}

static void test_templates_and_regex(void) {
  static const char *const T[] = {
      "IDENTIFIER",      "INTEGER",       "STRING",    "TEMPLATE_HEAD",
      "TEMPLATE_MIDDLE", "TEMPLATE_TAIL", "REGEX_LIT", "true",
      nullptr,
  };

  parse_grammar_t *g = mk(&JS, T);

  kinds(g, "a template with one hole", "`a${b}c`",
        "TEMPLATE_HEAD IDENTIFIER TEMPLATE_TAIL");

  kinds(g, "back-to-back holes", "`${a}${b}`",
        "TEMPLATE_HEAD IDENTIFIER TEMPLATE_MIDDLE IDENTIFIER "
        "TEMPLATE_TAIL");

  kinds(g, "a brace inside a hole does not close it", "`x${ {k:1} }y`",
        "TEMPLATE_HEAD { IDENTIFIER : INTEGER } TEMPLATE_TAIL");

  kinds(g, "a regex where an operand is expected", "x = /ab+/gi",
        "IDENTIFIER = REGEX_LIT");

  kinds(g, "a slash after a name is division", "a / b",
        "IDENTIFIER / IDENTIFIER");

  kinds(g, "a slash after a paren is division", "(1) / 2",
        "( INTEGER ) / INTEGER");

  kinds(g, "a slash after a literal keyword is division", "true / 2",
        "true / INTEGER");

  kinds(g, "a slash that never closes stays division", "a / b\nc",
        "IDENTIFIER / IDENTIFIER IDENTIFIER");

  kinds(g, "both comment forms are trivia", "a // x\n/* y */ b",
        "IDENTIFIER IDENTIFIER");

  parse_grammar_free(g);
}

static void test_sigils_and_heredocs(void) {
  static const char *const T[] = {
      "IDENTIFIER", "INTEGER", "HEREDOC", "CVAR", "IVAR", "GVAR", nullptr,
  };

  parse_grammar_t *g = mk(&RUBY, T);

  kinds(g, "the longest sigil wins", "@@a @b $c", "CVAR IVAR GVAR");
  kinds(g, "a bare sigil is not a sigil token", "@ 1", "@ INTEGER");

  kinds(g, "a heredoc body is one token and the line continues",
        "x = <<~EOS\n  body\nEOS\ny\n", "IDENTIFIER = HEREDOC IDENTIFIER");

  parse_grammar_free(g);
}

static void test_heredoc_families(void) {
  static const char *const SPEC =
      "@lex-heredoc-header(\"shell-word\", quotes(\"single\"), "
      "tag_start(\"_\"), tag_continue(\"_-\"))\n"
      "@lex-heredoc(\"<<\", header(\"shell-word\"), queue, "
      "interpolate(\"unquoted\", \"double\"), "
      "interpolated_as(\"INTERPOLATED\"), as(\"RAW\"), "
      "terminator_indent(\"allow\"), body_indent(\"strip\"))\n"
      "@lex-ident(as(\"IDENTIFIER\"))\n"
      "<file> ::= %IDENTIFIER | %INTERPOLATED | %RAW\n";
  core_string_t source = core_string_from_cstr(SPEC);
  parse_grammar_t *g = parse_grammar_new();

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, source);
  check("named heredoc header profiles load",
        parse_bnf_load(source, S("file"), g));

  const char *input = "call <<A <<'B'\n"
                      "  one\n"
                      "  A\n"
                      "    two\n"
                      "  B\n"
                      "after";
  parse_token_stream_t *ts = scan_tokenize_owned(g, input, strlen(input));
  parse_token_info_t *call = scan_stream_next(ts);
  parse_token_info_t *first = scan_stream_next(ts);
  parse_token_info_t *second = scan_stream_next(ts);
  parse_token_info_t *after = scan_stream_next(ts);

  check("queued heredocs retain source order",
        call && first && second && after && !scan_stream_next(ts) &&
            first->tid == parse_terminal_id(g, S("INTERPOLATED")) &&
            second->tid == parse_terminal_id(g, S("RAW")));
  check("queued bodies receive quote-sensitive policies",
        core_option_is_set(first->value) &&
            core_string_eq(core_option_get(first->value), S("one\n")) &&
            core_option_is_set(second->value) &&
            core_string_eq(core_option_get(second->value), S("two\n")));
  check("heredoc tokens retain body and label spans",
        first->has_content_span && first->has_label_span &&
            second->has_content_span && second->has_label_span);
  check("queued heredoc scanning resumes with following source",
        core_option_is_set(after->value) &&
            core_string_eq(core_option_get(after->value), S("after")) &&
            scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);

  scan_token_stream_free(ts);

  ts = scan_tokenize_incremental_owned(g);
  size_t token_count = 0;
  for (size_t i = 0; input[i]; i++) {
    check("queued heredoc input accepts a one-byte append",
          scan_stream_append(ts, input + i, 1));
    while (scan_stream_next(ts)) {
      token_count++;
    }
  }
  scan_stream_finish(ts);
  while (scan_stream_next(ts)) {
    token_count++;
  }
  check("queued heredocs resume across every header and body boundary",
        token_count == 4 && scan_status(ts->scanner) == SCAN_STATUS_COMPLETE &&
            scan_work_bytes(ts->scanner) < strlen(input) * 5);
  scan_token_stream_free(ts);

  const char *unicode_input = "x <<λ\nbody\nλ\n";
  ts = scan_tokenize_incremental_owned(g);
  token_count = 0;
  for (size_t i = 0; i < strlen(unicode_input); i++) {
    check("a Unicode heredoc tag accepts split UTF-8 input",
          scan_stream_append(ts, unicode_input + i, 1));
    while (scan_stream_next(ts)) {
      token_count++;
    }
  }
  scan_stream_finish(ts);
  while (scan_stream_next(ts)) {
    token_count++;
  }
  check("Unicode heredoc tags remain byte-exact terminators",
        token_count == 2 && scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);
  scan_token_stream_free(ts);

  ts = scan_tokenize_incremental_owned(g);
  check("queued headers can be consumed before their bodies arrive",
        scan_stream_append(ts, "x <<A <<B\n", strlen("x <<A <<B\n")) &&
            scan_stream_next(ts) && scan_stream_next(ts) &&
            scan_stream_next(ts));
  check("discarding an earlier queued token does not retain a raw pointer",
        scan_stream_discard_consumed(ts));
  check("discarded queued tokens do not impede later body scanning",
        scan_stream_append(ts, "one\nA\ntwo\nB\n", 12));
  scan_stream_finish(ts);
  while (scan_stream_next(ts)) {
  }
  check("queued scanning completes after consumed-token compaction",
        scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);
  scan_token_stream_free(ts);

  ts = scan_tokenize_owned(g, "x <<\"BAD\"\nBAD\n", 14);
  while (scan_stream_next(ts)) {
  }
  scan_diagnostic_t diagnostic = {0};
  check("disallowed heredoc quote forms are malformed headers",
        scan_diagnostic(ts->scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_INVALID &&
            strstr(diagnostic.message.data, "quoted heredoc tag"));
  scan_token_stream_free(ts);

  ts = scan_tokenize_owned(g, "x <<A\nbody\n", 11);
  while (scan_stream_next(ts)) {
  }
  diagnostic = (scan_diagnostic_t){0};
  check("missing queued bodies remain distinguishably incomplete",
        scan_diagnostic(ts->scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_INCOMPLETE &&
            strstr(diagnostic.message.data, "queued heredoc"));
  scan_token_stream_free(ts);

  parse_grammar_free(g);
  core_free(source.data);
}

static parse_grammar_t *literal_hook_grammar(void) {
  static const char *const SPEC =
      "@lex-string('\"', '\"', as(\"STRING\"))\n"
      "@lex-literal-hook(\"STRING\", hook(\"test-literal-hook\"), "
      "modifier(\"unit\"), outputs(\"SPECIAL\"), "
      "types(\"duration\"))\n"
      "<file> ::= %STRING | %SPECIAL\n";
  core_string_t source = core_string_from_cstr(SPEC);
  parse_grammar_t *g = parse_grammar_new();

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, source);
  bool loaded = parse_bnf_load(source, S("file"), g);
  core_free(source.data);

  if (!loaded) {
    parse_grammar_free(g);
    return nullptr;
  }
  return g;
}

static void test_literal_postprocessing_hooks(void) {
  check("literal hooks register by stable name",
        scan_lex_literal_hook_register_pure(S("test-literal-hook"),
                                            test_literal_hook));
  literal_hook_calls = 0;

  parse_grammar_t *g = literal_hook_grammar();
  check("declarative literal hook bindings load", g != nullptr);
  if (!g) {
    return;
  }

  parse_token_stream_t *ts =
      scan_tokenize_owned(g, "\"ok\" \"ok\"", strlen("\"ok\" \"ok\""));
  parse_token_info_t *first = scan_stream_next(ts);
  parse_token_info_t *second = scan_stream_next(ts);

  check("a hook may decode a value and select a declared token",
        first && second && !scan_stream_next(ts) &&
            first->tid == parse_terminal_id(g, S("SPECIAL")) &&
            second->tid == parse_terminal_id(g, S("SPECIAL")) &&
            core_option_is_set(first->value) &&
            core_string_eq(core_option_get(first->value), S("decoded")));
  check(
      "a hook may attach owned declared type metadata",
      core_option_is_set(first->literal_type) &&
          core_string_eq(core_option_get(first->literal_type), S("duration")));
  check("identical cacheable hook inputs execute once",
        literal_hook_calls == 1);
  scan_token_stream_free(ts);

  ts = scan_tokenize_owned(g, "\"bad\"", 5);
  while (scan_stream_next(ts)) {
  }
  scan_diagnostic_t diagnostic = {0};
  check("hook failures retain a relative narrow diagnostic span",
        scan_diagnostic(ts->scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_INVALID &&
            diagnostic.start_byte == 1 && diagnostic.end_byte == 4 &&
            core_string_eq(diagnostic.message, S("bad literal")));
  scan_token_stream_free(ts);

  ts = scan_tokenize_owned(g, "\"wrong\"", 7);
  while (scan_stream_next(ts)) {
  }
  diagnostic = (scan_diagnostic_t){0};
  check("hooks cannot select undeclared token metadata",
        scan_diagnostic(ts->scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_INVALID &&
            strstr(diagnostic.message.data, "undeclared token"));
  scan_token_stream_free(ts);
  parse_grammar_free(g);

  scan_lex_literal_result_t result = {0};
  scan_lex_literal_input_t input = {
      .spelling = S("{1, 2}"),
      .token = S("EMBED"),
      .modifier = S("set"),
  };
  check("compiler-side prefix tags can call the same generic hook API",
        scan_lex_literal_hook_call(S("test-literal-hook"), &input, &result) &&
            result.accepted && core_string_eq(result.value, S("set-value")) &&
            core_string_eq(result.type, S("set[int]")));
  scan_lex_literal_result_free(&result);

  check("literal hooks may compose without deadlocking the registry",
        scan_lex_literal_hook_register_pure(S("nested-literal-hook"),
                                            nested_literal_hook) &&
            scan_lex_literal_hook_register_pure(S("reentrant-literal-hook"),
                                                reentrant_literal_hook) &&
            scan_lex_literal_hook_call(S("reentrant-literal-hook"), &input,
                                       &result) &&
            result.accepted && core_string_eq(result.value, S("reentrant")));
  scan_lex_literal_result_free(&result);

  size_t calls_before_plain_scan = literal_hook_calls;
  parse_grammar_t *plain = mk(&PYTHON, PY_TERMS);
  ts = scan_tokenize_owned(plain, "123 \"plain\"", 11);
  while (scan_stream_next(ts)) {
  }
  check("lexers without hook bindings pay no callback cost",
        literal_hook_calls == calls_before_plain_scan);
  scan_token_stream_free(ts);
  parse_grammar_free(plain);

  static const char *const UNKNOWN =
      "@lex-string('\"', '\"', as(\"STRING\"))\n"
      "@lex-literal-hook(\"STRING\", hook(\"missing-hook\"))\n"
      "<file> ::= %STRING\n";
  core_string_t unknown_source = core_string_from_cstr(UNKNOWN);
  parse_grammar_t *unknown = parse_grammar_new();
  parse_grammar_set_error_recovery(unknown, false);
  parse_bnf_declare_terminals(unknown, unknown_source);
  bool loaded = parse_bnf_load(unknown_source, S("file"), unknown);
  check("unknown literal hooks fail during lexer construction",
        loaded && !scan_grammar_lex_compiled(unknown) &&
            scan_lex_compile_error() &&
            strstr(scan_lex_compile_error(), "unknown hook"));
  parse_grammar_free(unknown);
  core_free(unknown_source.data);
}

static void test_a_spec_is_reusable_across_runs(void) {
  parse_grammar_t *g = mk(&PYTHON, PY_TERMS);

  kinds(g, "the first run", "if x:\n  a\n",
        "if IDENTIFIER : NEWLINE INDENT IDENTIFIER NEWLINE DEDENT "
        "ENDMARKER");
  kinds(g, "and the state does not leak into the second", "if x:\n  a\n",
        "if IDENTIFIER : NEWLINE INDENT IDENTIFIER NEWLINE DEDENT "
        "ENDMARKER");

  parse_grammar_free(g);
}

static void test_a_grammar_declares_its_own_lexis(void) {
  static const char *const G =
      "@lex-comment(\"#\")\n"
      "@lex-comment(\"/*\", \"*/\", nested)\n"
      "@lex-string('\"', '\"', escape('\\\\'), as(\"STRING\"))\n"
      "@lex-string('f\"', '\"', escape('\\\\'), interp(\"{\", \"}\"), "
      "hole_tokens, "
      "as(\"FSTRING_START\", \"FSTRING_MIDDLE\", \"FSTRING_END\"))\n"
      "@lex-number(radix(\"0x\", \"0123456789abcdef\"), sep(\"_\"), "
      "exp(\"eE\"), int(\"INTEGER\"), float(\"FLOAT\"), "
      "suffix(\"j\", \"IMAGINARY\"))\n"
      "@lex-layout(indent(\"INDENT\", \"DEDENT\"), newline(\"NEWLINE\"), "
      "eof(\"ENDMARKER\"), continue(\"\\\\\"), brackets(\"()[]{}\"))\n"
      "@lex-ident(as(\"IDENTIFIER\"))\n"
      "<file> ::= %IDENTIFIER %NEWLINE %INDENT %DEDENT %ENDMARKER "
      "%INTEGER %FLOAT %STRING %IMAGINARY %FSTRING_START "
      "%FSTRING_MIDDLE %FSTRING_END %\"def\" %\"return\"\n";

  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_from_cstr(G);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);

  if (!parse_bnf_load(text, CORE_STRING_STATIC("file"), g)) {
    check("a grammar with @lex- declarations loads", false);
    parse_grammar_free(g);

    return;
  }

  kinds(g, "the declared lexis drives the scanner",
        "def f():\n    x = 0x1f + 2_0\n    return f\"a{x}b\"\n",
        "def IDENTIFIER ( ) : NEWLINE INDENT IDENTIFIER = INTEGER + "
        "INTEGER NEWLINE return FSTRING_START { IDENTIFIER } "
        "FSTRING_END NEWLINE DEDENT ENDMARKER");

  kinds(g, "a nested block comment closes once", "a /* x /* y */ z */ b",
        "IDENTIFIER IDENTIFIER NEWLINE ENDMARKER");

  check("nothing was rejected", scan_lex_build_error() == nullptr);

  parse_grammar_free(g);
  core_free(text.data);
}

static void test_string_families_expand_once(void) {
  static const char *const G =
      "@lex-string-family(plain, prefixes(\"r\", \"R\"), "
      "quotes(\"'\", '\"'), raw, as(\"STRING\"))\n"
      "<file> ::= %STRING\n";
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_from_cstr(G);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = parse_bnf_load(text, CORE_STRING_STATIC("file"), g);

  check("a declarative string family loads", loaded);
  kinds(g, "a string family expands prefixes across quote styles",
        "'a' \"b\" r'c' r\"d\" R'e' R\"f\"",
        "STRING_LIT STRING_LIT STRING_LIT STRING_LIT STRING_LIT "
        "STRING_LIT");

  scan_lex_info_t info = {0};
  check("a string family compiles its product once",
        scan_lex_inspect(scan_grammar_lex_compiled(g), &info) &&
            info.strings == 6);

  parse_token_stream_t *ts = scan_tokenize_incremental_owned(g);
  check("a family prefix waits across an incremental boundary",
        scan_stream_append(ts, "R", 1) && scan_stream_next(ts) == nullptr);
  check("a family string resumes without rescanning its prefix",
        scan_stream_append(ts, "\"body\" ", 7) &&
            scan_stream_next(ts) != nullptr);

  scan_token_stream_free(ts);
  parse_grammar_free(g);
  core_free(text.data);
}

static void test_string_families_reject_duplicate_openers(void) {
  static const char *const G =
      "@lex-string('r\"', '\"', raw, as(\"STRING\"))\n"
      "@lex-string-family(prefixes(\"r\"), quotes('\"'), raw, "
      "as(\"STRING\"))\n"
      "<file> ::= %STRING\n";
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_from_cstr(G);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = parse_bnf_load(text, CORE_STRING_STATIC("file"), g);
  scan_lex_t *lex = loaded ? scan_grammar_lex_compiled(g) : nullptr;
  const char *error = scan_lex_build_error();

  check("duplicate family openers fail during lexer compilation",
        loaded && !lex && error && strstr(error, "duplicate string opener") &&
            strstr(error, "r\""));

  parse_grammar_free(g);
  core_free(text.data);
}

static void family_error(const char *label, const char *source,
                         const char *expected) {
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_from_cstr(source);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = parse_bnf_load(text, CORE_STRING_STATIC("file"), g);
  scan_lex_t *lex = loaded ? scan_grammar_lex_compiled(g) : nullptr;
  const char *error = scan_lex_build_error();

  check(label, loaded && !lex && error && strstr(error, expected));

  parse_grammar_free(g);
  core_free(text.data);
}

static void test_string_families_diagnose_invalid_declarations(void) {
  family_error("a string family requires delimiter forms",
               "@lex-string-family(plain, raw, as(\"STRING\"))\n"
               "<file> ::= %STRING\n",
               "specify quotes() or pair()");
  family_error("a string family requires a prefix source",
               "@lex-string-family(quotes('\"'), raw, as(\"STRING\"))\n"
               "<file> ::= %STRING\n",
               "specify plain or prefixes()");
  family_error("a string family rejects positional shorthand",
               "@lex-string-family(\"r\", quotes('\"'), raw, "
               "as(\"STRING\"))\n<file> ::= %STRING\n",
               "use prefixes() and quotes()");
  family_error("a string family names an unknown option",
               "@lex-string-family(plain, quotes('\"'), bogus, "
               "as(\"STRING\"))\n<file> ::= %STRING\n",
               "bogus");
  family_error("a paired string family requires both delimiters",
               "@lex-string-family(prefixes(\"%q\"), pair(\"(\"), "
               "nested, as(\"Q\"))\n<file> ::= %Q\n",
               "pair() needs an opener and closer");
  family_error("symmetric strings reject nesting",
               "@lex-string-family(prefixes(\"%q\"), quotes(\"|\"), "
               "nested, as(\"Q\"))\n<file> ::= %Q\n",
               "nested strings need distinct paired delimiters");
}

static void test_paired_string_families_nest(void) {
  static const char *const G =
      "@lex-string-family(prefixes(\"%q\", \"%w\"), "
      "pair(\"(\", \")\"), pair(\"[\", \"]\"), "
      "pair(\"{\", \"}\"), pair(\"<\", \">\"), nested, "
      "multiline, escape(\"\\\\\"), as(\"Q\"))\n"
      "<file> ::= %Q\n";
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_from_cstr(G);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = parse_bnf_load(text, CORE_STRING_STATIC("file"), g);
  scan_lex_info_t info = {0};

  check("paired string families compile their delimiter product",
        loaded && scan_lex_inspect(scan_grammar_lex_compiled(g), &info) &&
            info.strings == 8);
  kinds(g, "paired strings balance their own delimiter",
        "%q(a(b)c) %q[a[b]c] %q{a{b}c} %q<a<b>c> "
        "%w(one(two)three)",
        "Q Q Q Q Q");
  kinds(g, "escaped paired delimiters do not affect nesting", "%q(a\\)b)", "Q");

  parse_token_stream_t *ts = scan_tokenize_incremental_owned(g);
  check(
      "nested paired strings retain depth across chunks",
      scan_stream_append(ts, "%q(a(b", 6) && scan_stream_next(ts) == nullptr &&
          scan_stream_append(ts, ")c) ", 4) && scan_stream_next(ts) != nullptr);
  scan_token_stream_free(ts);

  ts = scan_tokenize_incremental_owned(g);
  bool deep =
      scan_stream_append(ts, "%q(", 3) && scan_stream_next(ts) == nullptr;
  for (size_t i = 0; deep && i < 512; i++) {
    deep = scan_stream_append(ts, "(", 1) && scan_stream_next(ts) == nullptr;
  }
  for (size_t i = 0; deep && i < 512; i++) {
    deep = scan_stream_append(ts, ")", 1) && scan_stream_next(ts) == nullptr;
  }
  deep = deep && scan_stream_append(ts, ") ", 2) &&
         scan_stream_next(ts) != nullptr;
  check("deep paired nesting is iterative and incrementally resumable", deep);
  scan_token_stream_free(ts);

  check("an unbalanced paired string remains incomplete",
        source_status(g, "%q(a(b)c") == SCAN_STATUS_INCOMPLETE);

  parse_grammar_free(g);
  core_free(text.data);
}

static void test_composable_string_escape_profiles(void) {
  static const char *const G =
      "@lex-string-family(plain, quotes('\"', \"'\"), "
      "escape(\"\\\\\"), use_escapes(\"simple\", \"unicode\"), "
      "as(\"STRING\"))\n"
      "@lex-string-escapes(\"simple\", escapes(\"nrt\"), "
      "escape_digits(\"x\", \"0123456789abcdefABCDEF\", \"2\"))\n"
      "@lex-string-escapes(\"unicode\", "
      "escape_digits(\"u\", \"0123456789abcdefABCDEF\", \"4\"))\n"
      "<file> ::= %STRING\n";
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_from_cstr(G);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = parse_bnf_load(text, CORE_STRING_STATIC("file"), g);

  check("string escape profiles resolve before their users", loaded);
  check("composed escape profiles accept simple and fixed-width escapes",
        source_status(g, "\"\\n\\x4f\\u0041\"") == SCAN_STATUS_COMPLETE);
  check("composed escape profiles reject unknown escapes",
        source_status(g, "\"\\q\"") == SCAN_STATUS_INVALID);
  check("fixed-width escape profiles reject short bodies",
        source_status(g, "\"\\x4\"") == SCAN_STATUS_INVALID);

  parse_token_stream_t *ts = scan_tokenize_incremental_owned(g);
  check("a profiled escape waits across an incremental boundary",
        scan_stream_append(ts, "\"\\u00", 5) &&
            scan_stream_next(ts) == nullptr &&
            scan_stream_append(ts, "41\" ", 4) &&
            scan_stream_next(ts) != nullptr);
  scan_token_stream_free(ts);

  parse_grammar_free(g);
  core_free(text.data);

  family_error("string recipes name unknown escape profiles",
               "@lex-string('\"', '\"', escape(\"\\\\\"), "
               "use_escapes(\"missing\"), as(\"STRING\"))\n"
               "<file> ::= %STRING\n",
               "unknown escape set");
  family_error("escape profiles reject duplicate names",
               "@lex-string-escapes(\"same\", escapes(\"n\"))\n"
               "@lex-string-escapes(\"same\", escapes(\"r\"))\n"
               "<file> ::= %STRING\n",
               "duplicate set");
  family_error("composed profiles reject duplicate escape prefixes",
               "@lex-string-escapes(\"a\", "
               "escape_digits(\"u\", \"01\", \"2\"))\n"
               "@lex-string-escapes(\"b\", "
               "escape_digits(\"u\", \"01\", \"4\"))\n"
               "@lex-string('\"', '\"', escape(\"\\\\\"), "
               "use_escapes(\"a\", \"b\"), as(\"STRING\"))\n"
               "<file> ::= %STRING\n",
               "duplicate escape prefix");
  family_error("escape profiles validate unused duplicate rules",
               "@lex-string-escapes(\"bad\", "
               "escape_digits(\"u\", \"01\", \"2\"), "
               "escape_digits(\"u\", \"01\", \"4\"))\n"
               "<file> ::= %STRING\n",
               "duplicate escape prefix");
  family_error("escape profiles cannot be empty",
               "@lex-string-escapes(\"empty\")\n"
               "<file> ::= %STRING\n",
               "cannot be empty");
  family_error("fixed-width escape rules bound their streaming work",
               "@lex-string-escapes(\"wide\", "
               "escape_digits(\"hex\", \"01\", \"33\"))\n"
               "<file> ::= %STRING\n",
               "one-byte ASCII prefix and a width from 1 through 32");
}

static void test_large_string_family_dispatch_is_bounded(void) {
  core_buffer_t *source = core_buffer_empty();

  core_buffer_puts(source, "@lex-string-family(");
  for (size_t group = 0; group < 8; group++) {
    core_buffer_printf(source, "%sprefixes(", group ? ", " : "");
    for (size_t i = 0; i < 10; i++) {
      size_t prefix = group * 10 + i;
      core_buffer_printf(source, "%s\"p%03zu\"", i ? ", " : "", prefix);
    }
    core_buffer_putc(source, ')');
  }
  core_buffer_puts(source, ", quotes('\"'), raw, as(\"STRING\"))\n"
                           "<file> ::= %STRING\n");

  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = {
      .data = source->data,
      .u8_bytes = source->byte_len,
  };

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = parse_bnf_load(text, CORE_STRING_STATIC("file"), g);
  scan_lex_t *lex = loaded ? scan_grammar_lex_compiled(g) : nullptr;
  scan_lex_info_t info = {0};

  check("a large string family compiles",
        lex && scan_lex_inspect(lex, &info) && info.strings == 80);

  static const char input[] = "p079\"body\"";
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts =
      lex ? scan_tokenize(g, input, sizeof(input) - 1, nullptr, &sc) : nullptr;

  while (ts && scan_stream_next(ts)) {
  }

  check("string-family dispatch work is independent of family size",
        sc && scan_status(sc) == SCAN_STATUS_COMPLETE &&
            scan_work_bytes(sc) < 64);

  if (ts) {
    scan_token_stream_free(ts);
    scan_scanner_free(sc);
  }
  parse_grammar_free(g);
  core_buffer_free(source);
}

static void test_large_number_suffix_dispatch_is_bounded(void) {
  core_buffer_t *source = core_buffer_empty();

  for (size_t group = 0; group < 8; group++) {
    core_buffer_printf(source, "@lex-number-suffixes(\"set%zu\"", group);
    for (size_t i = 0; i < 10; i++) {
      core_buffer_printf(source, ", suffix(\"z%03zu\", \"INTEGER\")",
                         group * 10 + i);
    }
    core_buffer_puts(source, ")\n");
  }
  core_buffer_puts(source, "@lex-number(digits(\"0123456789\"), "
                           "use_suffixes(\"set0\", \"set1\", \"set2\", "
                           "\"set3\", \"set4\", \"set5\", \"set6\", "
                           "\"set7\"), int(\"INTEGER\"))\n"
                           "<file> ::= %INTEGER\n");

  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = {
      .data = source->data,
      .u8_bytes = source->byte_len,
  };
  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = parse_bnf_load(text, CORE_STRING_STATIC("file"), g);
  scan_lex_t *lex = loaded ? scan_grammar_lex_compiled(g) : nullptr;
  scan_lex_info_t info = {0};

  check("large composed numeric suffix sets compile",
        lex && scan_lex_inspect(lex, &info) && info.number_forms == 1 &&
            info.number_suffix_nodes > 80);

  static const char input[] = "1z079";
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts =
      lex ? scan_tokenize(g, input, sizeof(input) - 1, nullptr, &sc) : nullptr;
  while (ts && scan_stream_next(ts)) {
  }
  check("numeric suffix dispatch work is independent of set size",
        sc && scan_status(sc) == SCAN_STATUS_COMPLETE &&
            scan_work_bytes(sc) < 32);

  if (ts) {
    scan_token_stream_free(ts);
    scan_scanner_free(sc);
  }
  parse_grammar_free(g);
  core_buffer_free(source);
}

static void test_long_numeric_trie_paths_resume_linearly(void) {
  core_buffer_t *source = core_buffer_empty();

  core_buffer_puts(source, "@lex-number-suffixes(\"long\", suffix(\"");
  for (size_t i = 0; i < 512; i++) {
    core_buffer_putc(source, 'z');
  }
  core_buffer_puts(source, "\", \"INTEGER\"))\n"
                           "@lex-number(digits(\"0123456789\"), "
                           "use_suffixes(\"long\"), int(\"INTEGER\"))\n"
                           "@lex-number(prefix(\"");
  core_buffer_putc(source, '0');
  for (size_t i = 1; i < 512; i++) {
    core_buffer_putc(source, 'x');
  }
  core_buffer_puts(source, "\"), digits(\"0123456789\"), int(\"INTEGER\"))\n"
                           "<file> ::= %INTEGER\n");

  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = {
      .data = source->data,
      .u8_bytes = source->byte_len,
  };
  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = parse_bnf_load(text, CORE_STRING_STATIC("file"), g);

  parse_token_stream_t *ts =
      loaded ? scan_tokenize_incremental_owned(g) : nullptr;
  bool suffix_ok =
      ts && scan_stream_append(ts, "1", 1) && scan_stream_next(ts) == nullptr;
  for (size_t i = 0; suffix_ok && i < 512; i++) {
    suffix_ok =
        scan_stream_append(ts, "z", 1) && scan_stream_next(ts) == nullptr;
  }
  suffix_ok = suffix_ok && scan_stream_append(ts, " ", 1) &&
              scan_stream_next(ts) != nullptr &&
              scan_work_bytes(ts->scanner) < 2048;
  check("a long numeric suffix resumes in linear work", suffix_ok);
  scan_token_stream_free(ts);

  ts = loaded ? scan_tokenize_incremental_owned(g) : nullptr;
  bool prefix_ok =
      ts && scan_stream_append(ts, "0", 1) && scan_stream_next(ts) == nullptr;
  for (size_t i = 1; prefix_ok && i < 512; i++) {
    prefix_ok =
        scan_stream_append(ts, "x", 1) && scan_stream_next(ts) == nullptr;
  }
  prefix_ok = prefix_ok && scan_stream_append(ts, "1 ", 2) &&
              scan_stream_next(ts) != nullptr &&
              scan_work_bytes(ts->scanner) < 2048;
  check("a long numeric prefix resumes in linear work", prefix_ok);
  scan_token_stream_free(ts);

  parse_grammar_free(g);
  core_buffer_free(source);
}

static void test_an_unknown_option_is_named(void) {
  static const char *const G = "@lex-string(\"'\", \"'\", bogus)\n"
                               "<file> ::= %IDENTIFIER\n";

  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_from_cstr(G);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  parse_bnf_load(text, CORE_STRING_STATIC("file"), g);

  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;

  parse_token_stream_t *ts = scan_tokenize(g, "a", 1, &buf, &sc);

  const char *err = scan_lex_build_error();

  check("an unknown @lex- option is reported", err != nullptr);
  check("and the message names it",
        err && strstr(err, "bogus") && strstr(err, "lex-string"));

  if (ts) {
    scan_token_stream_free(ts);
    scan_scanner_free(sc);
    core_buffer_free(buf);
  }

  parse_grammar_free(g);
  core_free(text.data);

  static const char *const R =
      "@lex-regex(\"/\", \"/\", not_after(\"TYPO\"), as(\"REGEX\"))\n"
      "<file> ::= %REGEX\n";

  g = parse_grammar_new();
  text = core_string_from_cstr(R);
  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  parse_bnf_load(text, CORE_STRING_STATIC("file"), g);

  ts = scan_tokenize(g, "/x/", 3, &buf, &sc);
  err = scan_lex_build_error();

  check("an unknown regex predecessor token is rejected",
        err && strstr(err, "TYPO") && strstr(err, "not_after"));

  if (ts) {
    scan_token_stream_free(ts);
    scan_scanner_free(sc);
    core_buffer_free(buf);
  }

  parse_grammar_free(g);
  core_free(text.data);
}

static void test_lex_declarations_have_no_fixed_limit(void) {
  core_buffer_t *source = core_buffer_empty();

  for (size_t i = 0; i < 80; i++) {
    core_buffer_printf(source, "@lex-comment(\"#%zu\")\n", i);
    core_buffer_printf(source, "@lex-string(\"s%zu\")\n", i);
    core_buffer_printf(source,
                       "@lex-number(radix(\"r%zu\", \"01\"), "
                       "suffix(\"z%zu\"))\n",
                       i, i);
    core_buffer_printf(source, "@lex-sigil(\"$%zu\")\n", i);
    core_buffer_printf(source,
                       "@lex-regex(\"/%zu\", \"/\", "
                       "not_after(\"IDENTIFIER\"))\n",
                       i);
    core_buffer_printf(source, "@lex-heredoc(\"h%zu\")\n", i);
    core_buffer_printf(source, "@lex-embed(\"<%zu\", \">\")\n", i);
    core_buffer_printf(source,
                       "@lex-fence(\"[%zu\", \"]\", fill(\"=\"), "
                       "body(\"[\", \"]\"), as(\"FENCE\"))\n",
                       i);
    core_buffer_printf(source, "@lex-directive(\"d%zu\")\n", i);
  }

  core_buffer_puts(source, "<file> ::= %IDENTIFIER\n");

  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = {.data = source->data, .u8_bytes = source->byte_len};

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);

  bool loaded = parse_bnf_load(text, CORE_STRING_STATIC("file"), g);
  check("many lexer declarations load", loaded);

  const scan_lex_spec_t *spec =
      loaded ? scan_lex_spec_from_grammar(g) : nullptr;

  check("many lexer declarations build", spec != nullptr);
  if (spec) {
    check("all comments are retained",
          spec->comments[79].opener.data != nullptr);
    check("all strings are retained", spec->strings[79].opener.data != nullptr);
    check("all number radixes are retained",
          spec->number.radixes[79].prefix.data != nullptr);
    check("all number suffixes are retained",
          spec->number.suffixes[79].suffix.data != nullptr);
    check("all sigils are retained", spec->sigils[79].prefix.data != nullptr);
    check("all regexes are retained", spec->regexes[79].opener.data != nullptr);
    check("regex predecessor policies are retained",
          spec->regexes[79].not_after &&
              core_string_eq(spec->regexes[79].not_after[0], S("IDENTIFIER")) &&
              !spec->regexes[79].not_after[1].data);
    check("all heredocs are retained",
          spec->heredocs[79].opener.data != nullptr);
    check("all embeds are retained", spec->embeds[79].open.data != nullptr);
    check("all fences are retained",
          spec->fences[79].outer_open.data != nullptr);
    check("all directives are retained",
          spec->directives[79].open.data != nullptr);
    check("the dynamic comment list is terminated",
          spec->comments[80].opener.data == nullptr);
    check("the dynamic suffix list is terminated",
          spec->number.suffixes[80].suffix.data == nullptr);
  }

  scan_lex_spec_free(spec);
  parse_grammar_free(g);
  core_buffer_free(source);
}

static parse_grammar_t *fence_grammar(void) {
  static const char *const G =
      "@lex-fence(\"[\", \"]\", fill(\"=\"), body(\"[\", \"]\"), "
      "minimum(\"1\"), label, as(\"EMBED\"))\n"
      "<file> ::= %EMBED\n";

  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_from_cstr(G);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);

  if (!parse_bnf_load(text, CORE_STRING_STATIC("file"), g)) {
    parse_grammar_free(g);
    g = nullptr;
  }

  core_free(text.data);
  return g;
}

static parse_grammar_t *hash_fence_grammar(void) {
  static const char *const G =
      "@lex-fence(\"#\", \"#\", fill(\"#\"), body('\"', '\"'), "
      "minimum(\"0\"), as(\"RAW\"))\n"
      "@lex-sigil(\"r#\", as(\"IDENTIFIER\"))\n"
      "@lex-ident(as(\"IDENTIFIER\"))\n"
      "<file> ::= %RAW | %IDENTIFIER\n";

  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_from_cstr(G);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);

  if (!parse_bnf_load(text, CORE_STRING_STATIC("file"), g)) {
    parse_grammar_free(g);
    g = nullptr;
  }

  core_free(text.data);
  return g;
}

static void test_zero_minimum_hash_fences(void) {
  parse_grammar_t *g = hash_fence_grammar();

  kinds(g, "hash fences retain their exact delimiter width",
        "#\"one\"# ###\"two\"### r#name", "RAW RAW IDENTIFIER");

  core_buffer_t *source = core_buffer_empty();
  for (size_t i = 0; i < 4096; i++) {
    core_buffer_putc(source, '#');
  }
  core_buffer_puts(source, "if ");

  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts =
      scan_tokenize(g, source->data, source->byte_len, nullptr, &sc);
  while (scan_stream_next(ts)) {
  }

  check("a failed variable fence probe does linear scanner work",
        scan_status(sc) == SCAN_STATUS_COMPLETE &&
            scan_work_bytes(sc) < source->byte_len * 6);

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(source);
  parse_grammar_free(g);
}

static void test_symmetric_fences_preserve_source_spans(void) {
  const char *src = "[==codec[one ]=] two]==]";
  parse_grammar_t *g = fence_grammar();
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize(g, src, strlen(src), &buf, &sc);
  parse_token_info_t *tok = scan_stream_next(ts);

  check("a parameterized symmetric fence is one token",
        tok && scan_stream_next(ts) == nullptr);
  check("a fence retains its exact body span",
        tok && tok->has_content_span &&
            tok->content_end_byte - tok->content_start_byte == 11 &&
            !memcmp(src + tok->content_start_byte, "one ]=] two", 11));
  check("a fence retains its optional label span",
        tok && tok->has_label_span &&
            tok->label_end_byte - tok->label_start_byte == 5 &&
            !memcmp(src + tok->label_start_byte, "codec", 5));
  check("a closed fence is complete", scan_status(sc) == SCAN_STATUS_COMPLETE);

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
}

static void test_fence_padding_requires_both_boundaries(void) {
  static const char *const GRAMMAR =
      "@lex-fence(\"[\", \"]\", fill(\"-\"), pad(\"required\"), "
      "as(\"FENCE\"))\n"
      "@lex-fallback(\"reject\")\n"
      "<file> ::= %FENCE\n";
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_from_cstr(GRAMMAR);
  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = parse_bnf_load(text, S("file"), g);
  check("a padded fence declaration loads", loaded);
  core_free(text.data);
  if (!loaded) {
    parse_grammar_free(g);
    return;
  }

  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize(g, "[- -]", 5, &buf, &sc);
  parse_token_info_t *tok = scan_stream_next(ts);
  check("required padding accepts a space-padded empty literal",
        tok && tok->has_content_span &&
            tok->content_start_byte == tok->content_end_byte &&
            scan_stream_next(ts) == nullptr);
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  buf = nullptr;
  sc = nullptr;
  ts = scan_tokenize(g, "[- f-]", 6, &buf, &sc);
  bool emitted_fence = false;
  while ((tok = scan_stream_next(ts))) {
    emitted_fence |= tok->tid == parse_terminal_id(g, S("FENCE"));
  }
  check("required padding rejects a closing fence without boundary space",
        !emitted_fence && scan_status(sc) == SCAN_STATUS_INCOMPLETE);
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
}

static void test_unterminated_constructs_are_incomplete(void) {
  const char *src = "[=ffi[body";
  parse_grammar_t *g = fence_grammar();
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize(g, src, strlen(src), &buf, &sc);

  while (scan_stream_next(ts)) {
  }

  check("an unterminated fence is incomplete, not ordinary EOF",
        scan_status(sc) == SCAN_STATUS_INCOMPLETE);
  check("an incomplete scan identifies the construct's start",
        sc->status_start_byte == 0 && sc->status_end_byte == strlen(src));

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
}

static void test_incomplete_fences_resume_after_append(void) {
  const char *src = "[=ffi[body";
  parse_grammar_t *g = fence_grammar();
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize(g, src, strlen(src), &buf, &sc);

  check("an incomplete fence emits no provisional token",
        scan_stream_next(ts) == nullptr && scan_stream_token_count(ts) == 0);
  check("an incomplete stream accepts more source",
        scan_stream_append(ts, "]=]", 3));

  parse_token_info_t *tok = scan_stream_next(ts);

  check("the resumed fence is emitted once",
        tok && scan_stream_next(ts) == nullptr &&
            scan_stream_token_count(ts) == 1);
  check("the resumed fence retains its complete body span",
        tok && tok->has_content_span &&
            tok->content_end_byte - tok->content_start_byte == 4 &&
            !memcmp(buf->data + tok->content_start_byte, "body", 4));
  check("the resumed stream completes",
        scan_status(sc) == SCAN_STATUS_COMPLETE);
  check("a complete stream rejects further append",
        !scan_stream_append(ts, "x", 1));

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
}

static void test_incomplete_strings_and_comments_resume(void) {
  static const char *const TERMS[] = {
      "IDENTIFIER",
      "STRING",
      nullptr,
  };
  parse_grammar_t *g = mk(&JS, TERMS);
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize(g, "\"hel", 4, &buf, &sc);

  check("an incomplete string emits no provisional token",
        scan_stream_next(ts) == nullptr && scan_stream_token_count(ts) == 0);
  check("a string can remain incomplete across appends",
        scan_stream_append(ts, "lo", 2) && scan_stream_next(ts) == nullptr &&
            scan_status(sc) == SCAN_STATUS_INCOMPLETE);
  check("a later append can complete the string",
        scan_stream_append(ts, "\"", 1));

  parse_token_info_t *string = scan_stream_next(ts);

  check("the resumed string contains both source fragments",
        string && core_option_is_set(string->value) &&
            core_option_get(string->value).u8_bytes == 7 &&
            !memcmp(core_option_get(string->value).data, "\"hello\"", 7));

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  buf = nullptr;
  sc = nullptr;
  ts = scan_tokenize(g, "/* open", 7, &buf, &sc);

  check("an incomplete block comment emits no token",
        scan_stream_next(ts) == nullptr &&
            scan_status(sc) == SCAN_STATUS_INCOMPLETE);
  check("a block comment can resume after append",
        scan_stream_append(ts, " */ name", 8));

  parse_token_info_t *name = scan_stream_next(ts);

  check("scanning continues after a resumed block comment",
        name && core_option_is_set(name->value) &&
            core_string_eq(core_option_get(name->value),
                           CORE_STRING_STATIC("name")) &&
            name->leading_trivia &&
            core_string_eq(name->leading_trivia->text,
                           CORE_STRING_STATIC("/* open */")));

  while (scan_stream_next(ts)) {
  }

  check("the resumed comment stream completes",
        scan_status(sc) == SCAN_STATUS_COMPLETE);

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
}

static void test_filled_block_comment_delimiters(void) {
  static const char *const GRAMMAR = "@lex-comment(\"/\", \"/\", fill(\"*\"))\n"
                                     "@lex-ident(as(\"IDENTIFIER\"))\n"
                                     "<file> ::= %IDENTIFIER\n";
  parse_grammar_t *g = parse_grammar_new();
  core_string_t grammar_text = core_string_from_cstr(GRAMMAR);
  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, grammar_text);
  bool loaded = parse_bnf_load(grammar_text, S("file"), g);
  check("a repeated-fill block comment declaration loads", loaded);
  core_free(grammar_text.data);
  if (!loaded) {
    parse_grammar_free(g);
    return;
  }

  const char *source = "/** /* Hi! */ **/ name";
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts =
      scan_tokenize(g, source, strlen(source), &buf, &sc);
  parse_token_info_t *name = scan_stream_next(ts);
  check("the closing star count matches the opening star count",
        name && core_option_is_set(name->value) &&
            core_string_eq(core_option_get(name->value), S("name")) &&
            name->leading_trivia &&
            core_string_eq(name->leading_trivia->text, S("/** /* Hi! */ **/")));
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  const char *mismatched = "/** body */";
  buf = nullptr;
  sc = nullptr;
  ts = scan_tokenize(g, mismatched, strlen(mismatched), &buf, &sc);
  check("a closing star count that differs is not accepted",
        scan_stream_next(ts) == nullptr &&
            scan_status(sc) == SCAN_STATUS_INCOMPLETE);
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  buf = nullptr;
  sc = nullptr;
  ts = scan_tokenize(g, "/** body **", 11, &buf, &sc);
  check("a repeated-fill close may be split across input chunks",
        scan_stream_next(ts) == nullptr &&
            scan_status(sc) == SCAN_STATUS_INCOMPLETE &&
            scan_stream_append(ts, "/ name", 6));
  name = scan_stream_next(ts);
  check("the resumed filled comment closes at the matching count",
        name && core_option_is_set(name->value) &&
            core_string_eq(core_option_get(name->value), S("name")));
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
}

static void test_interpolation_regex_and_heredoc_resume(void) {
  static const char *const JS_TERMS[] = {
      "IDENTIFIER",    "TEMPLATE_HEAD", "TEMPLATE_MIDDLE",
      "TEMPLATE_TAIL", "REGEX_LIT",     nullptr,
  };
  parse_grammar_t *g = mk(&JS, JS_TERMS);
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize(g, "`a${x}", 6, &buf, &sc);
  parse_token_info_t *head = scan_stream_next(ts);
  parse_token_info_t *hole = scan_stream_next(ts);

  check("an interpolated tail can stop incomplete",
        head && hole && scan_stream_next(ts) == nullptr &&
            scan_status(sc) == SCAN_STATUS_INCOMPLETE);
  check("an interpolated tail can resume after append",
        scan_stream_append(ts, "b`", 2));

  parse_token_info_t *tail = scan_stream_next(ts);

  check("a resumed interpolation emits one tail",
        tail && scan_stream_next(ts) == nullptr &&
            scan_stream_token_count(ts) == 3 &&
            scan_status(sc) == SCAN_STATUS_COMPLETE);
  check("append retains completed token objects",
        scan_stream_get(ts, 0) == head && scan_stream_get(ts, 1) == hole);

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  buf = nullptr;
  sc = nullptr;
  ts = scan_tokenize(g, "/ab", 3, &buf, &sc);

  check("an unterminated regex emits no provisional token",
        scan_stream_next(ts) == nullptr &&
            scan_status(sc) == SCAN_STATUS_INCOMPLETE);
  check("a regex can resume after append", scan_stream_append(ts, "+/gi", 4));

  parse_token_info_t *regex = scan_stream_next(ts);

  check("the resumed regex is emitted once",
        regex && scan_stream_next(ts) == nullptr &&
            scan_stream_token_count(ts) == 1 &&
            scan_status(sc) == SCAN_STATUS_COMPLETE);

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);

  static const char *const RB_TERMS[] = {
      "IDENTIFIER",
      "HEREDOC",
      nullptr,
  };

  g = mk(&RUBY, RB_TERMS);
  buf = nullptr;
  sc = nullptr;
  ts = scan_tokenize(g, "<<~END\nbody\n", 12, &buf, &sc);

  check("an unterminated heredoc emits no provisional token",
        scan_stream_next(ts) == nullptr &&
            scan_status(sc) == SCAN_STATUS_INCOMPLETE &&
            sc->status_end_byte == 12);
  check("a heredoc can resume after append",
        scan_stream_append(ts, "END\nnext", 8));

  parse_token_info_t *heredoc = scan_stream_next(ts);
  parse_token_info_t *next = scan_stream_next(ts);

  check("the resumed heredoc preserves following source",
        heredoc && next && scan_stream_next(ts) == nullptr &&
            core_option_is_set(next->value) &&
            core_string_eq(core_option_get(next->value),
                           CORE_STRING_STATIC("next")));

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
}

static void test_incremental_stream_accepts_arbitrary_chunks(void) {
  static const char *const TERMS[] = {
      "IDENTIFIER", "INTEGER", "FLOAT", "STRING", nullptr,
  };
  parse_grammar_t *g = mk(&JS, TERMS);
  parse_register_terminal(g, core_string_from_cstr("==="));
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize_incremental(g, &buf, &sc);

  bool first_append = scan_stream_append(ts, "iden", 4);
  parse_token_info_t *first_token = scan_stream_next(ts);
  check("an identifier waits at a temporary buffer boundary",
        first_append && first_token == nullptr &&
            scan_status(sc) == SCAN_STATUS_INCOMPLETE);
  check("an identifier continues in the next chunk",
        scan_stream_append(ts, "t ", 2) && scan_stream_next(ts) != nullptr);

  check("a terminal prefix waits for a longer terminal",
        scan_stream_append(ts, "=", 1) && scan_stream_next(ts) == nullptr);
  check("the longest terminal completes across chunks",
        scan_stream_append(ts, "==", 2) && scan_stream_next(ts) != nullptr);

  check("a radix number waits across chunks",
        scan_stream_append(ts, " 0", 2) && scan_stream_next(ts) == nullptr &&
            scan_stream_append(ts, "x1", 2) && scan_stream_next(ts) == nullptr);
  parse_token_info_t *hex = nullptr;
  check("a radix number ends when a delimiter arrives",
        scan_stream_append(ts, "f ", 2) &&
            (hex = scan_stream_next(ts)) != nullptr &&
            core_option_get(hex->value).u8_bytes == 4);

  check("an exponent resumes after its sign",
        scan_stream_append(ts, "1.2e", 4) && scan_stream_next(ts) == nullptr &&
            scan_stream_append(ts, "+", 1) && scan_stream_next(ts) == nullptr &&
            scan_stream_append(ts, "3 ", 2) && scan_stream_next(ts) != nullptr);

  check("a line comment continues across chunks",
        scan_stream_append(ts, "// com", 6) &&
            scan_stream_next(ts) == nullptr &&
            scan_stream_append(ts, "ment\nname ", 10) &&
            scan_stream_next(ts) != nullptr);

  check("finishing converts a temporary boundary to final EOF",
        scan_stream_finish(ts) && scan_stream_next(ts) == nullptr &&
            scan_status(sc) == SCAN_STATUS_COMPLETE);
  check("a finished stream rejects more input",
        !scan_stream_append(ts, "x", 1));

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
}

static void test_incremental_utf8_and_source_compaction(void) {
  static const char *const TERMS[] = {
      "IDENTIFIER", "INTEGER", "FLOAT", "STRING", nullptr,
  };
  parse_grammar_t *g = mk(&JS, TERMS);
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize_incremental(g, &buf, &sc);
  const char lambda[] = "\xCE\xBB";

  check("a partial UTF-8 scalar waits for its remaining bytes",
        scan_stream_append(ts, lambda, 1) && scan_stream_next(ts) == nullptr &&
            sc->cursor == 0);
  check("a Unicode identifier resumes across a byte boundary",
        scan_stream_append(ts, lambda + 1, 1) &&
            scan_stream_next(ts) == nullptr && scan_stream_append(ts, " ", 1));

  parse_token_info_t *first = scan_stream_next(ts);
  check("the resumed Unicode token preserves its complete spelling",
        first && core_option_is_set(first->value) &&
            core_string_eq(core_option_get(first->value),
                           core_string_borrow(lambda, 2)));
  check("consumed incremental source can be discarded",
        scan_stream_discard_consumed(ts) && sc->source_base == 2 &&
            buf->byte_len == 1);

  parse_token_info_t *second = nullptr;
  check("absolute source spans survive compaction",
        scan_stream_append(ts, "name ", 5) &&
            (second = scan_stream_next(ts)) != nullptr);
  check("a post-compaction token uses the original byte coordinate space",
        second && second->start_byte == 3 && second->end_byte == 7);
  check("discarding consumed input also bounds the rewind cache",
        scan_stream_discard_consumed(ts) && ts->token_count <= 1);

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
}

static void test_source_compaction_is_amortized(void) {
  static const char *const TERMS[] = {
      "IDENTIFIER", "INTEGER", "FLOAT", "STRING", nullptr,
  };
  parse_grammar_t *g = mk(&JS, TERMS);
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize_incremental(g, &buf, &sc);
  core_buffer_t *source = core_buffer_empty();

  for (size_t i = 0; i < 4096; i++) {
    core_buffer_append(source, "a ", 2);
  }

  size_t original = source->byte_len;
  scan_stream_append(ts, source->data, source->byte_len);
  check("the first token in a large incremental window is available",
        scan_stream_next(ts) != nullptr);
  check("discard advances the logical window without shifting its suffix",
        scan_stream_discard_consumed(ts) && sc->input_offset == 1 &&
            sc->input_len == original - 1 && buf->byte_len == original);

  size_t tokens = 1;
  while (scan_stream_next(ts)) {
    tokens++;
    scan_stream_discard_consumed(ts);
  }

  check("geometric compaction preserves every token", tokens == 4096);
  check("geometric compaction bounds the retained source prefix",
        sc->input_offset < sc->input_len || sc->input_len <= 1);

  core_buffer_free(source);
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
}

static bool token_value_is(parse_token_info_t *token, const char *text) {
  if (!token || !core_option_is_set(token->value)) {
    return false;
  }
  core_string_t value = core_option_get(token->value);
  return value.u8_bytes == strlen(text) &&
         !memcmp(value.data, text, value.u8_bytes);
}

static void test_incremental_source_edits(void) {
  static const char *const TERMS[] = {
      "IDENTIFIER", "INTEGER", "FLOAT", "STRING", nullptr,
  };
  parse_grammar_t *g = mk(&JS, TERMS);
  core_buffer_t *source = core_buffer_empty();
  for (size_t i = 0; i < 80; i++) {
    core_buffer_printf(source, "a%zu ", i);
  }
  parse_token_stream_t *ts =
      scan_tokenize_owned(g, source->data, source->byte_len);
  check("source editing can be enabled before tokenization",
        scan_stream_enable_edits(ts));
  while (scan_stream_next(ts)) {
  }
  parse_token_info_t *stable_prefix = scan_stream_get(ts, 5);
  parse_token_info_t *stable_suffix = scan_stream_get(ts, 70);
  char *at = strstr(ts->scanner->buffer->data, "a40");
  size_t start = (size_t)(at - ts->scanner->buffer->data);

  check("a replacement restarts from a lexical checkpoint",
        scan_stream_replace(ts, start, start + 3, "renamed", 7));
  parse_token_info_t *changed = scan_stream_get(ts, 40);
  check("replacement preserves stable prefix token identities",
        scan_stream_get(ts, 5) == stable_prefix &&
            token_value_is(changed, "renamed"));
  scan_stream_get(ts, 1000);
  check("replacement reconciles unchanged suffix token identities",
        scan_stream_get(ts, 70) == stable_suffix);

  at = strstr(ts->scanner->buffer->data, "a41 ");
  start = (size_t)(at - ts->scanner->buffer->data);
  check("deletion retokenizes the affected suffix",
        scan_stream_delete(ts, start, start + 4) &&
            token_value_is(scan_stream_get(ts, 41), "a42"));

  at = strstr(ts->scanner->buffer->data, "a42");
  start = (size_t)(at - ts->scanner->buffer->data);
  check("insertion participates in normal longest-match scanning",
        scan_stream_insert(ts, start, "inserted ", 9) &&
            token_value_is(scan_stream_get(ts, 41), "inserted") &&
            token_value_is(scan_stream_get(ts, 42), "a42"));
  while (scan_stream_next(ts)) {
  }
  check("edited streams reach a complete scanner state",
        scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);
  scan_token_stream_free(ts);
  core_buffer_free(source);
  parse_grammar_free(g);

  g = mk(&JS, TERMS);
  ts = scan_tokenize_owned(g, "\"open", 5);
  scan_stream_enable_edits(ts);
  while (scan_stream_next(ts)) {
  }
  check("an edit can complete a formerly incomplete construct",
        scan_status(ts->scanner) == SCAN_STATUS_INCOMPLETE &&
            scan_stream_replace(ts, 5, 5, "\"", 1));
  scan_stream_get(ts, 1000);
  check("completion state is recomputed after insertion",
        scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);
  check("deleting a closer can make a construct incomplete again",
        scan_stream_replace(ts, 5, 6, nullptr, 0));
  scan_stream_get(ts, 1000);
  check("incomplete diagnostics are rebuilt after deletion",
        scan_status(ts->scanner) == SCAN_STATUS_INCOMPLETE);
  scan_token_stream_free(ts);
  parse_grammar_free(g);

  g = mk(&JS, TERMS);
  source = core_buffer_empty();
  for (size_t i = 0; i < 4096; i++) {
    core_buffer_puts(source, "aaaa ");
  }
  ts = scan_tokenize_owned(g, source->data, source->byte_len);
  scan_stream_enable_edits(ts);
  while (scan_stream_next(ts)) {
  }
  size_t work_before = scan_work_bytes(ts->scanner);
  bool bounded = true;
  size_t edit_at = 2048 * 5;
  for (size_t i = 0; i < 100; i++) {
    const char *byte = i & 1 ? "a" : "b";
    bounded = bounded &&
              scan_stream_replace(ts, edit_at, edit_at + 1, byte, 1) &&
              scan_stream_get(ts, 4095) != nullptr;
  }
  size_t edit_work = scan_work_bytes(ts->scanner) - work_before;
  check("repeated fixed-size edits converge without quadratic rescanning",
        bounded && edit_work < 100 * 1024);
  scan_token_stream_free(ts);
  core_buffer_free(source);
  parse_grammar_free(g);

  static const char *const JS_TERMS[] = {
      "IDENTIFIER",    "INTEGER",         "FLOAT",         "STRING",
      "TEMPLATE_HEAD", "TEMPLATE_MIDDLE", "TEMPLATE_TAIL", nullptr,
  };
  g = mk(&JS, JS_TERMS);
  source = core_buffer_empty();
  for (size_t i = 0; i < 30; i++) {
    core_buffer_puts(source, "p ");
  }
  core_buffer_puts(source, "`head${x}tail`");
  ts = scan_tokenize_owned(g, source->data, source->byte_len);
  scan_stream_enable_edits(ts);
  while (scan_stream_next(ts)) {
  }
  at = strstr(ts->scanner->buffer->data, "tail");
  start = (size_t)(at - ts->scanner->buffer->data);
  check("an edit resumes from checkpointed interpolation state",
        scan_stream_replace(ts, start, start + 4, "ending", 6));
  while (scan_stream_next(ts)) {
  }
  check("interpolation state recovers after an edit",
        scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);
  scan_token_stream_free(ts);
  core_buffer_free(source);
  parse_grammar_free(g);

  static const char *const PY_EDIT_TERMS[] = {
      "NEWLINE",        "INDENT",      "DEDENT", "ENDMARKER", "IDENTIFIER",
      "INTEGER",        "FLOAT",       "STRING", "IMAGINARY", "FSTRING_START",
      "FSTRING_MIDDLE", "FSTRING_END", nullptr,
  };
  g = mk(&PYTHON, PY_EDIT_TERMS);
  source = core_buffer_from_cstr("root\n  a\n  b\n  c\nend\n");
  ts = scan_tokenize_owned(g, source->data, source->byte_len);
  scan_stream_enable_edits(ts);
  while (scan_stream_next(ts)) {
  }
  at = strstr(ts->scanner->buffer->data, "  b");
  start = (size_t)(at - ts->scanner->buffer->data);
  check("indentation state is rebuilt after an edit",
        scan_stream_replace(ts, start, start + 2, "    ", 4));
  while (scan_stream_next(ts)) {
  }
  check("edited indentation produces a deterministic result",
        scan_status(ts->scanner) != SCAN_STATUS_INCOMPLETE);
  scan_token_stream_free(ts);
  core_buffer_free(source);
  parse_grammar_free(g);

  g = embedded_grammar("json", true);
  source = core_buffer_empty();
  for (size_t i = 0; i < 40; i++) {
    core_buffer_puts(source, "outer ");
  }
  core_buffer_puts(source, "{{1}} tail");
  ts = scan_tokenize_owned(g, source->data, source->byte_len);
  scan_stream_enable_edits(ts);
  while (scan_stream_next(ts)) {
  }
  at = strstr(ts->scanner->buffer->data, "{{1}}") + 2;
  start = (size_t)(at - ts->scanner->buffer->data);
  check("embedded lexer state recovers after an inner edit",
        scan_stream_replace(ts, start, start + 1, "22", 2));
  scan_stream_get(ts, 1000);
  check("an edited embedded stream converges and completes",
        scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);
  at = strstr(ts->scanner->buffer->data, "22");
  start = (size_t)(at - ts->scanner->buffer->data);
  check("a second inner edit can invalidate an embedded language",
        scan_stream_replace(ts, start, start + 2, "\"open", 5));
  scan_stream_get(ts, 1000);
  scan_diagnostic_t edited_embed_diagnostic = {0};
  check("post-edit inner diagnostics retain embedded lexer identity",
        scan_diagnostic(ts->scanner, &edited_embed_diagnostic) &&
            core_string_eq(edited_embed_diagnostic.lexer, S("json")));
  scan_token_stream_free(ts);
  core_buffer_free(source);
  parse_grammar_free(g);

  g = fence_grammar();
  const char *fence_source = "[==ffi[body]==]";
  ts = scan_tokenize_owned(g, fence_source, strlen(fence_source));
  scan_stream_enable_edits(ts);
  while (scan_stream_next(ts)) {
  }
  at = strstr(ts->scanner->buffer->data, "body");
  start = (size_t)(at - ts->scanner->buffer->data);
  check("an edit inside a variable-width fence invalidates that token",
        scan_stream_replace(ts, start, start + 4, "changed", 7));
  parse_token_info_t *fence = scan_stream_get(ts, 0);
  scan_stream_get(ts, 1000);
  check("fence state and content spans recover after an edit",
        fence && fence->has_content_span &&
            scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);
  scan_token_stream_free(ts);
  parse_grammar_free(g);

  static const char *const RB_EDIT_TERMS[] = {
      "IDENTIFIER", "INTEGER", "HEREDOC", "CVAR", "IVAR", "GVAR", nullptr,
  };
  g = mk(&RUBY, RB_EDIT_TERMS);
  const char *heredoc_source = "x = <<~END\n  body\nEND\ny\n";
  ts = scan_tokenize_owned(g, heredoc_source, strlen(heredoc_source));
  scan_stream_enable_edits(ts);
  while (scan_stream_next(ts)) {
  }
  at = strstr(ts->scanner->buffer->data, "body");
  start = (size_t)(at - ts->scanner->buffer->data);
  check("an edit inside a heredoc invalidates and rebuilds its body",
        scan_stream_replace(ts, start, start + 4, "revised", 7));
  scan_stream_get(ts, 1000);
  check("heredoc queues recover after an edit",
        scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);
  scan_token_stream_free(ts);
  parse_grammar_free(g);
}

static size_t scan_one_byte_chunks(parse_grammar_t *g, const char *source) {
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize_incremental(g, nullptr, &sc);
  size_t len = strlen(source);

  for (size_t i = 0; i < len; i++) {
    if (!scan_stream_append(ts, source + i, 1)) {
      break;
    }
    (void)scan_stream_next(ts);
  }
  scan_stream_finish(ts);
  while (scan_stream_next(ts)) {
  }

  size_t work = scan_work_bytes(sc);
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  return work;
}

static size_t count_one_byte_chunk_tokens(parse_grammar_t *g,
                                          const char *source,
                                          int32_t wanted_tid) {
  parse_token_stream_t *ts = scan_tokenize_incremental(g, nullptr, nullptr);
  size_t count = 0;

  for (size_t i = 0; source[i]; i++) {
    scan_stream_append(ts, source + i, 1);
    parse_token_info_t *tok = nullptr;
    while ((tok = scan_stream_next(ts))) {
      count += tok->tid == wanted_tid;
    }
  }
  scan_stream_finish(ts);
  parse_token_info_t *tok = nullptr;
  while ((tok = scan_stream_next(ts))) {
    count += tok->tid == wanted_tid;
  }
  scan_token_stream_free(ts);
  return count;
}

static void test_incremental_scanning_is_linear(void) {
  static const char *const TERMS[] = {
      "IDENTIFIER", "INTEGER", "FLOAT", "STRING", nullptr,
  };
  parse_grammar_t *js = mk(&JS, TERMS);
  parse_grammar_t *ruby = mk(&RUBY, TERMS);
  parse_grammar_t *fence = fence_grammar();
  parse_grammar_t *hash_fence = hash_fence_grammar();
  core_buffer_t *body = core_buffer_empty();

  for (size_t i = 0; i < 4096; i++) {
    core_buffer_putc(body, 'a');
  }

  core_buffer_t *string = core_buffer_from_cstr("\"");
  core_buffer_append(string, body->data, body->byte_len);
  core_buffer_puts(string, "\" ");
  core_buffer_t *comment = core_buffer_from_cstr("//");
  core_buffer_append(comment, body->data, body->byte_len);
  core_buffer_puts(comment, "\nname ");
  core_buffer_t *heredoc = core_buffer_from_cstr("<<~END\n");
  core_buffer_append(heredoc, body->data, body->byte_len);
  core_buffer_puts(heredoc, "\nEND\nname ");
  core_buffer_t *fenced = core_buffer_from_cstr("[=ffi[");
  core_buffer_append(fenced, body->data, body->byte_len);
  core_buffer_puts(fenced, "]=] ");
  core_buffer_t *hash_fenced = core_buffer_from_cstr("###\"");
  core_buffer_append(hash_fenced, body->data, body->byte_len);
  core_buffer_puts(hash_fenced, "\"### ");

  check("one-byte string chunks take linear scanner work",
        scan_one_byte_chunks(js, string->data) < string->byte_len * 5);
  check("one-byte comment chunks take linear scanner work",
        scan_one_byte_chunks(js, comment->data) < comment->byte_len * 5);
  check("one-byte heredoc chunks take linear scanner work",
        scan_one_byte_chunks(ruby, heredoc->data) < heredoc->byte_len * 5);
  check("one-byte fenced chunks take linear scanner work",
        scan_one_byte_chunks(fence, fenced->data) < fenced->byte_len * 5);
  check("one-byte variable hash fences take linear scanner work",
        scan_one_byte_chunks(hash_fence, hash_fenced->data) <
            hash_fenced->byte_len * 5);
  check("a split comment opener is not mistaken for a regex",
        count_one_byte_chunk_tokens(js, "//body\n", PARSE_TOK_STRING_LIT) == 0);
  check("a split string opener still produces one string token",
        count_one_byte_chunk_tokens(
            js, "\"body\"", (int32_t)parse_terminal_id(js, S("STRING"))) == 1);
  check("a split heredoc opener still produces one heredoc token",
        count_one_byte_chunk_tokens(
            ruby, "<<~END\nbody\nEND\n",
            (int32_t)parse_terminal_id(ruby, S("HEREDOC"))) == 1);
  check("a split fence opener still produces one fence token",
        count_one_byte_chunk_tokens(
            fence, "[=ffi[body]=]",
            (int32_t)parse_terminal_id(fence, S("EMBED"))) == 1);

  core_buffer_free(string);
  core_buffer_free(comment);
  core_buffer_free(heredoc);
  core_buffer_free(fenced);
  core_buffer_free(hash_fenced);
  core_buffer_free(body);
  parse_grammar_free(js);
  parse_grammar_free(ruby);
  parse_grammar_free(fence);
  parse_grammar_free(hash_fence);
}

static void test_convenience_tokenization_owns_its_resources(void) {
  static const char *const TERMS[] = {"IDENTIFIER", nullptr};
  parse_grammar_t *g = mk(&JS, TERMS);
  parse_token_stream_t *ts = scan_tokenize_owned(g, "name", 4);

  check("convenience tokenization returns a usable owned stream",
        ts && scan_stream_next(ts) != nullptr);
  scan_token_stream_free(ts);

  ts = scan_tokenize_incremental_owned(g);
  check("owned incremental tokenization needs no side-channel handles",
        ts && scan_stream_append(ts, "name ", 5) &&
            scan_stream_next(ts) != nullptr);
  scan_token_stream_free(ts);
  parse_grammar_free(g);
}

static parse_grammar_t *soft_image_grammar(void) {
  static const char *const SPEC =
      "@lex-ident(as(\"IDENTIFIER\"))\n"
      "@lex-soft-keywords(\"match\", \"case\")\n"
      "@lex-fallback(\"reject\")\n"
      "<file> ::= %\"match\" %IDENTIFIER %\"case\"\n";
  core_string_t text = core_string_from_cstr(SPEC);
  parse_grammar_t *grammar = parse_grammar_new();

  parse_grammar_set_error_recovery(grammar, false);
  parse_bnf_declare_terminals(grammar, text);
  if (!parse_bnf_load(text, S("file"), grammar)) {
    parse_grammar_free(grammar);
    grammar = nullptr;
  }
  core_free(text.data);
  return grammar;
}

static void test_generated_scanner_images(void) {
  static const char *const TERMS[] = {
      "IDENTIFIER",    "INTEGER",       "FLOAT",
      "STRING",        "TEMPLATE_HEAD", "TEMPLATE_MIDDLE",
      "TEMPLATE_TAIL", "REGEX_LIT",     nullptr,
  };
  parse_grammar_t *source = mk(&JS, TERMS);
  scan_lex_t *compiled = scan_grammar_lex_compiled(source);
  core_string_t first = scan_lex_image_encode(compiled);
  core_string_t second = scan_lex_image_encode(compiled);
  parse_grammar_t *equivalent = mk(&JS, TERMS);
  core_string_t independently_encoded =
      scan_lex_image_encode(scan_grammar_lex_compiled(equivalent));

  check("compiled scanner images encode successfully",
        first.data && first.u8_bytes > 0 && !scan_lex_image_error());
  check("compiled scanner image emission is deterministic",
        first.u8_bytes == second.u8_bytes &&
            !memcmp(first.data, second.data, first.u8_bytes));
  check("equivalent independently compiled scanners emit identical images",
        first.u8_bytes == independently_encoded.u8_bytes &&
            !memcmp(first.data, independently_encoded.data, first.u8_bytes));

  parse_grammar_t *loaded = bare_grammar(TERMS);
  check("a scanner image installs without a registered specification",
        scan_grammar_set_lex_image(loaded, first));
  if (loaded->lex_compiled) {
    scan_lex_info_t source_info = {0};
    scan_lex_info_t loaded_info = {0};
    scan_lex_inspect(compiled, &source_info);
    scan_lex_inspect(loaded->lex_compiled, &loaded_info);
    check("loaded scanner images retain their compiled table sizes",
          source_info.terminal_nodes == loaded_info.terminal_nodes &&
              source_info.terminal_edges == loaded_info.terminal_edges &&
              source_info.opener_nodes == loaded_info.opener_nodes &&
              source_info.number_prefix_nodes ==
                  loaded_info.number_prefix_nodes &&
              source_info.number_suffix_nodes ==
                  loaded_info.number_suffix_nodes);
    kinds(loaded, "loaded scanner images produce the runtime scanner's tokens",
          "name = 0x1f + /ab+c/i; `x${name}` // tail\n",
          "IDENTIFIER = INTEGER + REGEX_LIT ; TEMPLATE_HEAD IDENTIFIER "
          "TEMPLATE_TAIL");
  }

  parse_grammar_t *mismatch = bare_grammar(TERMS);
  parse_register_terminal(mismatch, S("EXTRA"));
  check("scanner images reject a different terminal vocabulary",
        !scan_grammar_set_lex_image(mismatch, first) &&
            scan_lex_image_error() &&
            strstr(scan_lex_image_error(), "vocabulary") != nullptr);

  parse_grammar_t *codepoint_source = mk(&JS, TERMS);
  parse_nonterm_t *source_file = parse_nonterm(codepoint_source, S("file"));
  parse_add_rule(codepoint_source, source_file, PARSE_CHAR('+'));
  core_string_t codepoint_image =
      scan_lex_image_encode(scan_grammar_lex_compiled(codepoint_source));
  parse_grammar_t *codepoint_target = bare_grammar(TERMS);
  parse_nonterm_t *target_file = parse_nonterm(codepoint_target, S("file"));
  parse_add_rule(codepoint_target, target_file, PARSE_CHAR('-'));
  check("scanner images bind codepoint terminal vocabularies",
        !scan_grammar_set_lex_image(codepoint_target, codepoint_image) &&
            scan_lex_image_error() &&
            strstr(scan_lex_image_error(), "vocabulary") != nullptr);
  core_free(codepoint_image.data);
  parse_grammar_free(codepoint_target);
  parse_grammar_free(codepoint_source);

  char *damaged_data = core_alloc_size(1, first.u8_bytes);
  memcpy(damaged_data, first.data, first.u8_bytes);
  damaged_data[first.u8_bytes / 2] ^= 0x5a;
  core_string_t damaged = core_string_borrow(damaged_data, first.u8_bytes);
  parse_grammar_t *damaged_target = bare_grammar(TERMS);
  check("scanner images reject corrupted payloads",
        !scan_grammar_set_lex_image(damaged_target, damaged) &&
            scan_lex_image_error() &&
            strstr(scan_lex_image_error(), "checksum") != nullptr);

  memcpy(damaged_data, first.data, first.u8_bytes);
  damaged_data[first.u8_bytes - 1] ^= 0x5a;
  check("scanner images checksum relocation metadata",
        !scan_grammar_set_lex_image(damaged_target, damaged) &&
            scan_lex_image_error() &&
            strstr(scan_lex_image_error(), "checksum") != nullptr);

  memcpy(damaged_data, first.data, first.u8_bytes);
  test_lex_image_header_t *changed_root = (void *)damaged_data;
  changed_root->root_offset ^= sizeof(void *);
  check("scanner images checksum header metadata",
        !scan_grammar_set_lex_image(damaged_target, damaged) &&
            scan_lex_image_error() &&
            strstr(scan_lex_image_error(), "checksum") != nullptr);

  memcpy(damaged_data, first.data, first.u8_bytes);
  test_lex_image_header_t *oversized = (void *)damaged_data;
  oversized->payload_bytes = UINT64_MAX;
  check("scanner images reject overflowing section lengths",
        !scan_grammar_set_lex_image(damaged_target, damaged) &&
            scan_lex_image_error() &&
            strstr(scan_lex_image_error(), "bounds") != nullptr);

  core_free(damaged_data);
  core_free(first.data);
  core_free(second.data);
  core_free(independently_encoded.data);
  parse_grammar_free(damaged_target);
  parse_grammar_free(mismatch);
  parse_grammar_free(loaded);
  parse_grammar_free(source);
  parse_grammar_free(equivalent);

  parse_grammar_t *fence_source = fence_grammar();
  parse_grammar_t *fence_loaded = fence_grammar();
  core_string_t fence_image =
      scan_lex_image_encode(scan_grammar_lex_compiled(fence_source));
  check("scanner images preserve variable-width fence dispatch",
        scan_grammar_set_lex_image(fence_loaded, fence_image) &&
            source_status(fence_loaded, "[==ffi[body]==]") ==
                SCAN_STATUS_COMPLETE);
  core_free(fence_image.data);
  parse_grammar_free(fence_loaded);
  parse_grammar_free(fence_source);

  parse_grammar_t *embed_source = embedded_grammar("json", true);
  parse_grammar_t *embed_loaded = embedded_grammar("json", true);
  core_string_t embed_image =
      scan_lex_image_encode(scan_grammar_lex_compiled(embed_source));
  check("scanner images preserve nested compiled lexer graphs",
        scan_grammar_set_lex_image(embed_loaded, embed_image) &&
            source_status(embed_loaded, "a {{1 \"x\"}} b") ==
                SCAN_STATUS_COMPLETE);
  core_free(embed_image.data);
  parse_grammar_free(embed_loaded);
  parse_grammar_free(embed_source);

  static const char *const RUBY_TERMS[] = {
      "IDENTIFIER", "INTEGER", "HEREDOC", "CVAR", "IVAR", "GVAR", nullptr,
  };
  parse_grammar_t *heredoc_source = mk(&RUBY, RUBY_TERMS);
  parse_grammar_t *heredoc_loaded = mk(&RUBY, RUBY_TERMS);
  core_string_t heredoc_image =
      scan_lex_image_encode(scan_grammar_lex_compiled(heredoc_source));
  check("scanner images preserve heredoc recipes",
        scan_grammar_set_lex_image(heredoc_loaded, heredoc_image) &&
            source_status(heredoc_loaded, "x = <<~END\nbody\nEND\n") ==
                SCAN_STATUS_COMPLETE);
  core_free(heredoc_image.data);
  parse_grammar_free(heredoc_loaded);
  parse_grammar_free(heredoc_source);

  parse_grammar_t *hook_source = literal_hook_grammar();
  parse_grammar_t *hook_loaded = literal_hook_grammar();
  core_string_t hook_image =
      scan_lex_image_encode(scan_grammar_lex_compiled(hook_source));
  check("scanner images resolve registered pure literal hooks",
        scan_grammar_set_lex_image(hook_loaded, hook_image) &&
            source_status(hook_loaded, "\"value\"") == SCAN_STATUS_COMPLETE);
  core_free(hook_image.data);
  parse_grammar_free(hook_loaded);
  parse_grammar_free(hook_source);

  parse_grammar_t *unicode_source = unicode_identifier_grammar(nullptr);
  parse_grammar_t *unicode_loaded = unicode_identifier_grammar(nullptr);
  core_string_t unicode_image =
      scan_lex_image_encode(scan_grammar_lex_compiled(unicode_source));
  check("scanner images reacquire compatible Unicode providers",
        scan_grammar_set_lex_image(unicode_loaded, unicode_image) &&
            source_status(unicode_loaded, "caf\xC3\xA9") ==
                SCAN_STATUS_COMPLETE);
  core_free(unicode_image.data);
  parse_grammar_free(unicode_loaded);
  parse_grammar_free(unicode_source);

  parse_grammar_t *soft_source = soft_image_grammar();
  parse_grammar_t *soft_loaded = soft_image_grammar();
  core_string_t soft_image =
      scan_lex_image_encode(scan_grammar_lex_compiled(soft_source));
  bool soft_installed = scan_grammar_set_lex_image(soft_loaded, soft_image);
  parse_token_stream_t *soft_tokens =
      soft_installed
          ? scan_tokenize_owned(soft_loaded, "match subject case", 18)
          : nullptr;
  parse_pwz_parser_t *soft_parser =
      soft_installed
          ? parse_pwz_new_for_start(
                soft_loaded,
                parse_nonterm_id(parse_find_nonterm(soft_loaded, S("file"))))
          : nullptr;
  check("scanner images restore contextual-keyword preparation",
        soft_parser && soft_tokens &&
            parse_pwz_parse(soft_parser, soft_tokens));
  parse_pwz_free(soft_parser);
  scan_token_stream_free(soft_tokens);
  core_free(soft_image.data);
  parse_grammar_free(soft_loaded);
  parse_grammar_free(soft_source);
}

static void test_terminals_have_no_byte_width_limit(void) {
  static const char *const TERMS[] = {"IDENTIFIER", nullptr};
  parse_grammar_t *g = mk(&JS, TERMS);
  core_buffer_t *op = core_buffer_empty();

  for (size_t i = 0; i < 1024; i++) {
    core_buffer_putc(op, '<');
  }
  parse_register_terminal(g, core_string_borrow(op->data, op->byte_len));

  scan_lex_t *lex = scan_grammar_lex_compiled(g);
  scan_lex_info_t info = {0};
  check("terminal width is not capped by an eight-bit field",
        scan_lex_operator_width(lex, '<') == 1024);
  check("compiled lexer structure is available for inspection",
        scan_lex_inspect(lex, &info) && info.max_terminal_bytes == 1024 &&
            info.terminal_nodes >= 1025 && info.terminal_edges >= 1024);

  parse_token_stream_t *ts =
      scan_tokenize(g, op->data, op->byte_len, nullptr, nullptr);
  parse_token_info_t *tok = scan_stream_next(ts);
  check("the terminal trie matches a terminal longer than 255 bytes",
        tok && core_option_is_set(tok->value) &&
            core_option_get(tok->value).u8_bytes == 1024);

  scan_token_stream_free(ts);
  parse_grammar_free(g);
  core_buffer_free(op);
}

static void test_scanner_diagnostics_have_a_stable_view(void) {
  static const char *const TERMS[] = {
      "IDENTIFIER", "INTEGER", "FLOAT", "STRING", nullptr,
  };
  parse_grammar_t *g = mk(&JS, TERMS);
  parse_token_stream_t *ts = scan_tokenize_owned(g, "\"open", 5);

  while (scan_stream_next(ts)) {
  }

  scan_diagnostic_t diagnostic = {0};
  check("incomplete scanner diagnostics are inspectable",
        scan_diagnostic(ts->scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_INCOMPLETE &&
            diagnostic.message.data && diagnostic.start_byte == 0 &&
            diagnostic.end_byte == 5);

  scan_token_stream_free(ts);
  parse_grammar_free(g);
}

static void *finalize_concurrently(void *arg) {
  parse_grammar_finalize(arg);
  return nullptr;
}

static void test_grammar_finalization_is_thread_safe(void) {
  parse_grammar_t *g = fence_grammar();
  pthread_t threads[8];

  for (size_t i = 0; i < 8; i++) {
    pthread_create(&threads[i], nullptr, finalize_concurrently, g);
  }
  for (size_t i = 0; i < 8; i++) {
    pthread_join(threads[i], nullptr);
  }

  check("concurrent grammar finalization produces one complete grammar",
        g->finalized && g->lex_compiled != nullptr);
  parse_grammar_free(g);
}

static scan_status_t source_status(parse_grammar_t *g, const char *source) {
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts =
      scan_tokenize(g, source, strlen(source), &buf, &sc);

  while (scan_stream_next(ts)) {
  }

  scan_status_t result = scan_status(sc);

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  return result;
}

static void test_numeric_recipes_reject_malformed_forms(void) {
  static const char *const TERMS[] = {
      "IDENTIFIER", "INTEGER", "FLOAT", "IMAGINARY", nullptr,
  };
  parse_grammar_t *g = mk(&PYTHON, TERMS);

  check("numeric recipes accept well-formed separators and exponents",
        source_status(g, "0x1f 1_000 1.5e+2") == SCAN_STATUS_COMPLETE);
  check("a radix prefix requires at least one digit",
        source_status(g, "0x") == SCAN_STATUS_INVALID);
  check("a separator must occur between digits",
        source_status(g, "1__2") == SCAN_STATUS_INVALID);
  check("an exponent marker requires exponent digits",
        source_status(g, "1e+") == SCAN_STATUS_INVALID);

  parse_grammar_free(g);
}

static bool recovery_declaration_fails(const char *source,
                                       const char *expected) {
  core_string_t text = core_string_from_cstr(source);
  parse_grammar_t *grammar = parse_grammar_new();

  parse_grammar_set_error_recovery(grammar, false);
  parse_bnf_declare_terminals(grammar, text);
  bool loaded = parse_bnf_load(text, S("file"), grammar);
  scan_lex_t *program = loaded ? scan_grammar_lex_compiled(grammar) : nullptr;
  const char *error = scan_lex_build_error();
  if (!error) {
    error = scan_lex_compile_error();
  }
  bool result = !program && error && strstr(error, expected);

  core_free(text.data);
  parse_grammar_free(grammar);
  return result;
}

static void test_malformed_token_recovery(void) {
  static const char *const TERMS[] = {
      "IDENTIFIER", "INTEGER", "STRING", "ERROR", ";", nullptr,
  };
  const char *source = "1__oops; next";
  parse_grammar_t *strict = mk(&RECOVERY_LEXER, TERMS);
  parse_token_stream_t *ts =
      scan_tokenize_owned(strict, source, strlen(source));

  while (scan_stream_next(ts)) {
  }
  check("batch scanning remains strict when recovery is declared",
        scan_status(ts->scanner) == SCAN_STATUS_INVALID);
  scan_token_stream_free(ts);
  parse_grammar_free(strict);

  parse_grammar_t *editor = mk(&RECOVERY_LEXER, TERMS);
  core_buffer_t *buffer = nullptr;
  parse_scanner_t *scanner = nullptr;
  ts = scan_tokenize(editor, source, strlen(source), &buffer, &scanner);
  scan_scanner_set_recovery(scanner, true);

  parse_token_info_t *error = scan_stream_next(ts);
  parse_token_info_t *semicolon = scan_stream_next(ts);
  parse_token_info_t *identifier = scan_stream_next(ts);
  parse_token_info_t *end = scan_stream_next(ts);
  int64_t error_tid = parse_lookup_terminal(editor, S("ERROR"));
  int64_t semicolon_tid = parse_lookup_terminal(editor, S(";"));

  check("editor recovery replaces the malformed token",
        error && error->tid == error_tid && core_option_is_set(error->value) &&
            core_option_get(error->value).u8_bytes == 7 &&
            !memcmp(core_option_get(error->value).data, "1__oops", 7));
  check("recovery stops before an unconsumed synchronization token",
        semicolon && semicolon->tid == semicolon_tid && identifier &&
            identifier->tid == parse_lookup_terminal(editor, S("IDENTIFIER")));
  check("recovered input reaches EOF with a distinct status",
        !end && scan_status(scanner) == SCAN_STATUS_RECOVERED);

  scan_diagnostic_t diagnostic = {0};
  check("recovery retains the original malformed-token diagnostic",
        scan_diagnostic(scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_RECOVERED &&
            strstr(diagnostic.message.data, "numeric") != nullptr &&
            diagnostic.start_byte == 0);

  scan_token_stream_free(ts);
  scan_scanner_free(scanner);
  core_buffer_free(buffer);
  parse_grammar_free(editor);

  editor = mk(&RECOVERY_LEXER, TERMS);
  buffer = nullptr;
  scanner = nullptr;
  source = "\"bad\\q\" trailing\nnext";
  ts = scan_tokenize(editor, source, strlen(source), &buffer, &scanner);
  scan_scanner_set_recovery(scanner, true);
  error = scan_stream_next(ts);
  identifier = scan_stream_next(ts);
  end = scan_stream_next(ts);

  check("string recovery synchronizes without consuming later lines",
        error && error->tid == parse_lookup_terminal(editor, S("ERROR")) &&
            error->end_byte == 16 && identifier &&
            core_option_is_set(identifier->value) &&
            core_string_eq(core_option_get(identifier->value), S("next")) &&
            !end && scan_status(scanner) == SCAN_STATUS_RECOVERED);

  scan_token_stream_free(ts);
  scan_scanner_free(scanner);
  core_buffer_free(buffer);
  parse_grammar_free(editor);

  parse_grammar_t *incremental = mk(&MULTI_RECOVERY_LEXER, TERMS);
  buffer = nullptr;
  scanner = nullptr;
  ts = scan_tokenize_incremental(incremental, &buffer, &scanner);
  scan_scanner_set_recovery(scanner, true);
  bool first_chunk = scan_stream_append(ts, "1__oops;", 8);
  error = scan_stream_next(ts);
  check("a split synchronization delimiter keeps recovery incomplete",
        first_chunk && !error && scan_stream_token_count(ts) == 0 &&
            scan_status(scanner) == SCAN_STATUS_INCOMPLETE);
  bool second_chunk = scan_stream_append(ts, "; next", 6);
  bool finished = scan_stream_finish(ts);
  error = scan_stream_next(ts);
  while (scan_stream_next(ts)) {
  }
  check("recovery resumes across a split synchronization delimiter",
        second_chunk && finished && error &&
            error->tid == parse_lookup_terminal(incremental, S("ERROR")) &&
            core_option_is_set(error->value) &&
            core_option_get(error->value).u8_bytes == 7 &&
            scan_status(scanner) == SCAN_STATUS_RECOVERED);
  scan_token_stream_free(ts);
  scan_scanner_free(scanner);
  core_buffer_free(buffer);
  parse_grammar_free(incremental);

  parse_grammar_t *consuming = mk(&CONSUMING_RECOVERY_LEXER, TERMS);
  buffer = nullptr;
  scanner = nullptr;
  source = "1__oops;next";
  ts = scan_tokenize(consuming, source, strlen(source), &buffer, &scanner);
  scan_scanner_set_recovery(scanner, true);
  error = scan_stream_next(ts);
  identifier = scan_stream_next(ts);
  end = scan_stream_next(ts);
  check("a recovery recipe may consume its synchronization delimiter",
        error && error->tid == parse_lookup_terminal(consuming, S("ERROR")) &&
            error->end_byte == 8 && identifier &&
            core_option_is_set(identifier->value) &&
            core_string_eq(core_option_get(identifier->value), S("next")) &&
            !end && scan_status(scanner) == SCAN_STATUS_RECOVERED);
  scan_token_stream_free(ts);
  scan_scanner_free(scanner);
  core_buffer_free(buffer);
  parse_grammar_free(consuming);

  core_buffer_t *large_recovery = core_buffer_from_cstr("1__");
  for (size_t i = 0; i < 1 << 18; i++) {
    core_buffer_putc(large_recovery, 'a');
  }
  core_buffer_puts(large_recovery, "; next");
  editor = mk(&RECOVERY_LEXER, TERMS);
  buffer = nullptr;
  scanner = nullptr;
  ts = scan_tokenize(editor, large_recovery->data, large_recovery->byte_len,
                     &buffer, &scanner);
  scan_scanner_set_recovery(scanner, true);
  while (scan_stream_next(ts)) {
  }
  check("recovery synchronization remains linear on long malformed tokens",
        scan_status(scanner) == SCAN_STATUS_RECOVERED &&
            scan_work_bytes(scanner) < large_recovery->byte_len * 3);
  scan_token_stream_free(ts);
  scan_scanner_free(scanner);
  core_buffer_free(buffer);
  parse_grammar_free(editor);
  core_buffer_free(large_recovery);

  editor = mk(&RECOVERY_LEXER, TERMS);
  source = "1__oops; next";
  ts = scan_tokenize_owned(editor, source, strlen(source));
  bool recovery_enabled = scan_scanner_set_recovery(ts->scanner, true);
  bool edits_enabled = scan_stream_enable_edits(ts);
  while (scan_stream_next(ts)) {
  }
  bool initially_recovered = scan_status(ts->scanner) == SCAN_STATUS_RECOVERED;
  bool repaired = scan_stream_replace(ts, 0, 7, "1", 1);
  scan_stream_get(ts, 1000);
  check("editing can repair a recovered token and clear recovery status",
        recovery_enabled && edits_enabled && initially_recovered && repaired &&
            scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);
  scan_token_stream_free(ts);
  parse_grammar_free(editor);

  editor = mk(&REJECT_RECOVERY_LEXER, TERMS);
  buffer = nullptr;
  scanner = nullptr;
  ts = scan_tokenize(editor, "@", 1, &buffer, &scanner);
  scan_scanner_set_recovery(scanner, true);
  while (scan_stream_next(ts)) {
  }
  check("recovery mode leaves undeclared failures invalid",
        scan_status(scanner) == SCAN_STATUS_INVALID);
  scan_token_stream_free(ts);
  scan_scanner_free(scanner);
  core_buffer_free(buffer);
  parse_grammar_free(editor);

  parse_grammar_t *image_source = mk(&RECOVERY_LEXER, TERMS);
  core_string_t image =
      scan_lex_image_encode(scan_grammar_lex_compiled(image_source));
  parse_grammar_t *image_target = bare_grammar(TERMS);
  bool image_loaded = scan_grammar_set_lex_image(image_target, image);
  buffer = nullptr;
  scanner = nullptr;
  source = "1__oops; next";
  ts = image_loaded ? scan_tokenize(image_target, source, strlen(source),
                                    &buffer, &scanner)
                    : nullptr;
  if (scanner) {
    scan_scanner_set_recovery(scanner, true);
  }
  error = ts ? scan_stream_next(ts) : nullptr;
  while (ts && scan_stream_next(ts)) {
  }
  check("scanner images containing recovery tables load", image_loaded);
  check("generated scanner images preserve recovery tables",
        image_loaded && error &&
            error->tid == parse_lookup_terminal(image_target, S("ERROR")) &&
            scan_status(scanner) == SCAN_STATUS_RECOVERED);
  scan_token_stream_free(ts);
  scan_scanner_free(scanner);
  core_buffer_free(buffer);
  core_free(image.data);
  parse_grammar_free(image_target);
  parse_grammar_free(image_source);

  static const char *const GRAMMAR_TEXT =
      "@lex-number(digits(\"0123456789\"), sep(\"_\"), "
      "int(\"INTEGER\"))\n"
      "@lex-ident(as(\"IDENTIFIER\"))\n"
      "@lex-recover(\"INTEGER\", as(\"ERROR\"), sync(\";\"))\n"
      "@lex-fallback(\"reject\")\n"
      "<file> ::= %ERROR %\";\" %IDENTIFIER\n";
  core_string_t grammar_text = core_string_from_cstr(GRAMMAR_TEXT);
  parse_grammar_t *declared = parse_grammar_new();
  parse_grammar_set_error_recovery(declared, false);
  parse_bnf_declare_terminals(declared, grammar_text);
  bool grammar_loaded = parse_bnf_load(grammar_text, S("file"), declared);
  core_free(grammar_text.data);

  buffer = nullptr;
  scanner = nullptr;
  source = "1__oops; next";
  ts = grammar_loaded
           ? scan_tokenize(declared, source, strlen(source), &buffer, &scanner)
           : nullptr;
  if (scanner) {
    scan_scanner_set_recovery(scanner, true);
  }
  error = ts ? scan_stream_next(ts) : nullptr;
  while (ts && scan_stream_next(ts)) {
  }
  check("BNF lexer specifications accept declarative recovery recipes",
        grammar_loaded && error &&
            error->tid == parse_lookup_terminal(declared, S("ERROR")) &&
            scan_status(scanner) == SCAN_STATUS_RECOVERED);
  scan_token_stream_free(ts);
  scan_scanner_free(scanner);
  core_buffer_free(buffer);
  parse_grammar_free(declared);

  check("recovery declarations reject unknown token names",
        recovery_declaration_fails("@lex-ident(as(\"IDENTIFIER\"))\n"
                                   "@lex-recover(\"MISSING\", as(\"ERROR\"))\n"
                                   "<file> ::= %IDENTIFIER | %ERROR\n",
                                   "unknown token"));
  check(
      "recovery declarations reject duplicate input tokens",
      recovery_declaration_fails("@lex-ident(as(\"IDENTIFIER\"))\n"
                                 "@lex-recover(\"IDENTIFIER\", as(\"ERROR\"))\n"
                                 "@lex-recover(\"IDENTIFIER\", as(\"ERROR\"))\n"
                                 "<file> ::= %IDENTIFIER | %ERROR\n",
                                 "duplicate input token"));
  static const core_string_t EMPTY_SYNC[] = {S(""), {}};
  static const scan_lex_recovery_t EMPTY_RECOVERY[] = {
      {.token = S("IDENTIFIER"),
       .replacement = S("ERROR"),
       .synchronize = EMPTY_SYNC},
      {},
  };
  static const scan_lex_spec_t EMPTY_RECOVERY_SPEC = {
      .name = S("empty-recovery-test"),
      .recoveries = EMPTY_RECOVERY,
      .tok_ident = S("IDENTIFIER"),
  };
  parse_grammar_t *invalid = bare_grammar(TERMS);
  scan_lex_t *invalid_program = scan_lex_compile(&EMPTY_RECOVERY_SPEC, invalid);
  check("recovery declarations reject empty synchronization delimiters",
        !invalid_program && scan_lex_compile_error() &&
            strstr(scan_lex_compile_error(), "cannot be empty"));
  scan_lex_free(invalid_program);
  parse_grammar_free(invalid);
}

static parse_grammar_t *number_form_grammar(void) {
  static const char *const G =
      "@lex-number(digits(\"0123456789\"), sep(\"_\"), "
      "point(\"leading\", \"trailing\"), exp(\"eE\"), "
      "int(\"INTEGER\"), float(\"FLOAT\"), "
      "use_suffixes(\"imaginary\"))\n"
      "@lex-number(prefix(\"0x\"), "
      "digits(\"0123456789abcdefABCDEF\"), sep(\"_\"), "
      "point(\"trailing\", \"exponent-required\"), exp(\"pP\"), "
      "int(\"INTEGER\"), float(\"FLOAT\"))\n"
      "@lex-number(prefix(\"0b\"), digits(\"01\"), sep(\"_\"), "
      "int(\"INTEGER\"))\n"
      "@lex-number-suffixes(\"imaginary\", "
      "suffix(\"j\", \"IMAGINARY\"), "
      "suffix(\"imag\", \"IMAGINARY\"), "
      "suffix(\"u\", \"INTEGER\", \"integer\"), suffix(\"L\"))\n"
      "<file> ::= %INTEGER | %FLOAT | %IMAGINARY\n";
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_from_cstr(G);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  if (!parse_bnf_load(text, CORE_STRING_STATIC("file"), g)) {
    parse_grammar_free(g);
    g = nullptr;
  }
  core_free(text.data);
  return g;
}

static void test_self_contained_number_forms(void) {
  parse_grammar_t *g = number_form_grammar();

  kinds(g, "number forms own their point and exponent policies",
        "0 1. .5 1e2 0x1 0x1p2 0x1.fp2 0b101",
        "INTEGER FLOAT FLOAT FLOAT INTEGER FLOAT FLOAT INTEGER");
  kinds(g, "suffixes belong to one number form", "1j 0x1", "IMAGINARY INTEGER");
  kinds(g, "a suffix without a token preserves the form's token kind",
        "1L 1.0L", "INTEGER FLOAT");
  check("a prefixed form requires one of its own digits",
        source_status(g, "0x") == SCAN_STATUS_INVALID);
  check("a form rejects digits outside its alphabet",
        source_status(g, "0b102") == SCAN_STATUS_INVALID);
  check("a hexadecimal point requires its own exponent",
        source_status(g, "0x1.2") == SCAN_STATUS_INVALID);
  check("a suffix from another form is rejected",
        source_status(g, "0x1j") == SCAN_STATUS_INVALID);
  check("an integer-only suffix rejects a floating shape",
        source_status(g, "1.0u") == SCAN_STATUS_INVALID);

  parse_token_stream_t *ts = scan_tokenize_incremental_owned(g);
  check("a numeric suffix waits while a longer suffix can match",
        scan_stream_append(ts, "1i", 2) && scan_stream_next(ts) == nullptr);
  check("a numeric suffix resumes through its compiled trie",
        scan_stream_append(ts, "mag ", 4) && scan_stream_next(ts) != nullptr);
  scan_token_stream_free(ts);

  ts = scan_tokenize_incremental_owned(g);
  bool linear = true;
  for (size_t i = 0; i < 4096; i++) {
    linear = linear && scan_stream_append(ts, "1", 1) &&
             scan_stream_next(ts) == nullptr;
  }
  linear = linear && scan_stream_append(ts, " ", 1) &&
           scan_stream_next(ts) != nullptr &&
           scan_work_bytes(ts->scanner) < 8192;
  check("one-byte numeric streaming remains linear", linear);
  scan_token_stream_free(ts);

  parse_grammar_free(g);
}

static void number_error(const char *label, const char *source,
                         const char *expected) {
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_from_cstr(source);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = parse_bnf_load(text, CORE_STRING_STATIC("file"), g);
  scan_lex_t *lex = loaded ? scan_grammar_lex_compiled(g) : nullptr;
  const char *error = scan_lex_build_error();

  check(label, loaded && !lex && error && strstr(error, expected));
  parse_grammar_free(g);
  core_free(text.data);
}

static void test_number_forms_diagnose_invalid_declarations(void) {
  number_error("a number form requires a digit alphabet",
               "@lex-number(prefix(\"0x\"), int(\"INTEGER\"))\n"
               "<file> ::= %INTEGER\n",
               "requires digits()");
  number_error("number forms reject duplicate prefixes",
               "@lex-number(digits(\"0123456789\"), int(\"INTEGER\"))\n"
               "@lex-number(digits(\"01\"), int(\"INTEGER\"))\n"
               "<file> ::= %INTEGER\n",
               "duplicate number prefix");
  number_error("number forms name unknown point policies",
               "@lex-number(digits(\"0123456789\"), point(\"around\"), "
               "int(\"INTEGER\"))\n<file> ::= %INTEGER\n",
               "unknown point policy");
  number_error("number forms name unknown suffix sets",
               "@lex-number(digits(\"0123456789\"), "
               "use_suffixes(\"missing\"), int(\"INTEGER\"))\n"
               "<file> ::= %INTEGER\n",
               "unknown suffix set");
  number_error("composed suffix sets reject duplicate spellings",
               "@lex-number-suffixes(\"a\", suffix(\"u\", \"INTEGER\"))\n"
               "@lex-number-suffixes(\"b\", suffix(\"u\", \"INTEGER\"))\n"
               "@lex-number(digits(\"0123456789\"), "
               "use_suffixes(\"a\", \"b\"), int(\"INTEGER\"))\n"
               "<file> ::= %INTEGER\n",
               "duplicate suffix");
}

static void test_reusable_lexical_packages(void) {
  static const char *const G =
      "@lex-operator-set(\"derived-ops\", use(\"base-ops\"), "
      "remove(\"!=\"), remove(\"==\"), add(\"==\", \"??\"))\n"
      "@lex-string-escapes(\"derived-escapes\", "
      "use(\"base-escapes\"), remove(\"n\", \"t\"), "
      "escapes(\"rt\"))\n"
      "@lex-number-suffixes(\"derived-suffixes\", "
      "use(\"base-suffixes\"), remove(\"u\", \"v\"), "
      "suffix(\"v\", \"INTEGER\"), suffix(\"z\", \"INTEGER\"))\n"
      "@lex-operator-set(\"base-ops\", \"==\", \"!=\")\n"
      "@lex-string-escapes(\"base-escapes\", escapes(\"nt\"), "
      "escape_digits(\"x\", \"0123456789abcdef\", \"2\"))\n"
      "@lex-number-suffixes(\"base-suffixes\", "
      "suffix(\"u\", \"INTEGER\"), suffix(\"v\", \"INTEGER\"))\n"
      "@lex-string('\"', '\"', escape(\"\\\\\"), "
      "use_escapes(\"derived-escapes\"), as(\"STRING\"))\n"
      "@lex-number(digits(\"0123456789\"), int(\"INTEGER\"), "
      "use_suffixes(\"derived-suffixes\"))\n"
      "@lex-operators(use(\"derived-ops\"))\n"
      "@lex-fallback(\"reject\")\n"
      "<file> ::= %STRING | %INTEGER | %\"==\" | %\"??\"\n";
  core_string_t text = core_string_from_cstr(G);
  parse_grammar_t *g = parse_grammar_new();

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = parse_bnf_load(text, S("file"), g);
  check("lexical packages resolve forward and compose by ordered set algebra",
        loaded && scan_grammar_lex_compiled(g));
  if (loaded) {
    kinds(g, "composed packages expand before scanner construction",
          "\"\\r\\t\" 1v 1z == ??", "STRING_LIT INTEGER INTEGER == ??");
    check("subtracted escape recipes are absent",
          source_status(g, "\"\\n\"") == SCAN_STATUS_INVALID);
    check("subtracted numeric suffixes are absent",
          source_status(g, "1u") == SCAN_STATUS_INVALID);
    check("subtracted operators are absent",
          source_status(g, "!=") == SCAN_STATUS_INVALID);
  }
  parse_grammar_free(g);
  core_free(text.data);

  family_error("packages reject removal of absent members",
               "@lex-operator-set(\"bad\", remove(\"+\"))\n"
               "<file> ::= %\"+\"\n",
               "remove absent operator");
  family_error("packages diagnose unknown dependencies",
               "@lex-string-escapes(\"a\", use(\"missing\"))\n"
               "<file> ::= %STRING\n",
               "uses unknown package");
  family_error("packages diagnose dependency cycles",
               "@lex-number-suffixes(\"a\", use(\"b\"))\n"
               "@lex-number-suffixes(\"b\", use(\"a\"))\n"
               "<file> ::= %INTEGER\n",
               "dependency cycle");
}

typedef struct {
  parse_grammar_t *grammar;
  parse_nt_id_t start;
  const char *source;
  bool parsed;
} soft_keyword_parse_t;

static void *parse_soft_keyword_start(void *arg) {
  soft_keyword_parse_t *run = arg;
  parse_token_stream_t *ts =
      scan_tokenize_owned(run->grammar, run->source, strlen(run->source));
  parse_pwz_parser_t *parser =
      parse_pwz_new_for_start(run->grammar, run->start);

  run->parsed = parser && ts && parse_pwz_parse(parser, ts);
  parse_pwz_free(parser);
  scan_token_stream_free(ts);
  return nullptr;
}

static void test_soft_keywords(void) {
  static const char *const G =
      "@lex-ident(as(\"IDENTIFIER\"))\n"
      "@lex-soft-keywords(\"match\", \"case\")\n"
      "@lex-fallback(\"reject\")\n"
      "<statement> ::= %\"match\" %IDENTIFIER %\"case\"\n"
      "<name> ::= %IDENTIFIER\n";
  core_string_t text = core_string_from_cstr(G);
  parse_grammar_t *g = parse_grammar_new();

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = parse_bnf_load(text, S("statement"), g);
  check("soft-keyword declarations construct a lexer", loaded);

  if (loaded) {
    parse_token_stream_t *tokens =
        scan_tokenize_owned(g, "match case ordinary", 19);
    parse_token_info_t *match = scan_stream_next(tokens);
    parse_token_info_t *case_word = scan_stream_next(tokens);
    parse_token_info_t *ordinary = scan_stream_next(tokens);

    check("soft keywords retain the ordinary identifier token kind",
          match && case_word && ordinary &&
              match->tid == PARSE_TOK_IDENTIFIER &&
              case_word->tid == PARSE_TOK_IDENTIFIER &&
              ordinary->tid == PARSE_TOK_IDENTIFIER);
    scan_token_stream_free(tokens);

    parse_nonterm_t *statement = parse_find_nonterm(g, S("statement"));
    parse_nonterm_t *name = parse_find_nonterm(g, S("name"));
    soft_keyword_parse_t runs[] = {
        {.grammar = g,
         .start = parse_nonterm_id(statement),
         .source = "match subject case"},
        {.grammar = g, .start = parse_nonterm_id(name), .source = "match"},
    };
    pthread_t threads[2];

    pthread_create(&threads[0], nullptr, parse_soft_keyword_start, &runs[0]);
    pthread_create(&threads[1], nullptr, parse_soft_keyword_start, &runs[1]);
    pthread_join(threads[0], nullptr);
    pthread_join(threads[1], nullptr);
    check("soft-keyword spelling matches across parallel start symbols",
          runs[0].parsed && runs[1].parsed);

    soft_keyword_parse_t wrong = {
        .grammar = g,
        .start = parse_nonterm_id(statement),
        .source = "other subject case",
    };
    parse_soft_keyword_start(&wrong);
    check("contextual terminals still require their declared spelling",
          !wrong.parsed);

    parse_token_stream_t *ts = scan_tokenize_owned(g, "match subject case", 18);
    parse_pwz_parser_t *parser =
        parse_pwz_new_for_start(g, parse_nonterm_id(statement));
    bool parsed = parser && parse_pwz_parse(parser, ts);
    parse_token_info_t *first =
        parsed ? parse_tree_find_first_terminal(parse_pwz_get_tree(parser))
               : nullptr;
    check("parse trees preserve the original soft-keyword token",
          first && first->tid == PARSE_TOK_IDENTIFIER &&
              core_option_is_set(first->value) &&
              core_string_eq(core_option_get(first->value), S("match")));
    parse_pwz_free(parser);
    scan_token_stream_free(ts);
  }

  parse_grammar_free(g);
  core_free(text.data);

  family_error("soft keywords must name grammar terminals",
               "@lex-soft-keywords(\"missing\")\n"
               "<file> ::= %IDENTIFIER\n",
               "not a named grammar terminal");
  family_error("soft keyword spellings cannot be duplicated",
               "@lex-soft-keywords(\"match\", \"match\")\n"
               "<file> ::= %\"match\"\n",
               "duplicate spelling");
  family_error("soft keywords cannot also be fixed operators",
               "@lex-soft-keywords(\"match\")\n"
               "@lex-operators(add(\"match\"))\n"
               "<file> ::= %\"match\"\n",
               "also an active operator");
}

static void test_strings_distinguish_incomplete_from_invalid(void) {
  static const char *const PY_TERMS[] = {
      "IDENTIFIER",
      "STRING",
      nullptr,
  };
  static const char *const JS_TERMS[] = {
      "IDENTIFIER",      "STRING",        "TEMPLATE_HEAD",
      "TEMPLATE_MIDDLE", "TEMPLATE_TAIL", nullptr,
  };
  parse_grammar_t *py = mk(&PYTHON, PY_TERMS);
  parse_grammar_t *js = mk(&JS, JS_TERMS);

  check("a single-line string at EOF is incomplete",
        source_status(py, "\"unfinished") == SCAN_STATUS_INCOMPLETE);
  check("a newline in a single-line string is invalid",
        source_status(py, "\"bad\n") == SCAN_STATUS_INVALID);
  check("an interpolated string tail at EOF is incomplete",
        source_status(js, "`a${x}") == SCAN_STATUS_INCOMPLETE);

  parse_grammar_free(py);
  parse_grammar_free(js);
}

static void test_reject_fallback_accepts_declared_codepoints_only(void) {
  static const char *const G = "@lex-fallback(\"reject\")\n"
                               "<file> ::= %\"+\"\n";
  core_string_t text = core_string_from_cstr(G);
  parse_grammar_t *g = parse_grammar_new();

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  check("a rejecting fallback grammar loads",
        parse_bnf_load(text, CORE_STRING_STATIC("file"), g));

  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize(g, "+@", 2, &buf, &sc);
  parse_token_info_t *declared = scan_stream_next(ts);
  parse_token_info_t *unknown = scan_stream_next(ts);

  check("reject fallback retains declared codepoint terminals",
        declared && declared->tid == '+');
  check("reject fallback emits progress for an undeclared codepoint",
        unknown && unknown->tid == PARSE_TOK_OTHER);
  check("reject fallback reports invalid input",
        scan_status(sc) == SCAN_STATUS_INVALID && sc->status_start_byte == 1);

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
  core_free(text.data);
}

static parse_grammar_t *unicode_identifier_grammar(const char *version) {
  core_buffer_t *source = core_buffer_empty();

  core_buffer_printf(
      source,
      "@lex-ident-profile(\"safe\"%s%s%s, normalize(\"nfc\"), "
      "scripts(\"Latin\", \"Greek\"), mixed_scripts(\"diagnose\"), "
      "confusables(\"allow\"), emoji(\"diagnose\"))\n"
      "@lex-ident(as(\"IDENTIFIER\"), profile(\"safe\"))\n"
      "<file> ::= %%IDENTIFIER\n",
      version ? ", unicode(\"" : "", version ? version : "",
      version ? "\")" : "");
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_borrow(source->data, source->byte_len);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  if (!parse_bnf_load(text, CORE_STRING_STATIC("file"), g)) {
    parse_grammar_free(g);
    g = nullptr;
  }
  core_buffer_free(source);
  return g;
}

static parse_grammar_t *unicode_confusable_grammar(void) {
  static const char *const G =
      "@lex-ident-profile(\"confusable\", "
      "scripts(\"Latin\", \"Cyrillic\"), "
      "mixed_scripts(\"allow\"), confusables(\"diagnose\"))\n"
      "@lex-ident(as(\"IDENTIFIER\"), profile(\"confusable\"))\n"
      "<file> ::= %IDENTIFIER\n";
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_from_cstr(G);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  if (!parse_bnf_load(text, CORE_STRING_STATIC("file"), g)) {
    parse_grammar_free(g);
    g = nullptr;
  }
  core_free(text.data);
  return g;
}

static void test_unicode_identifier_profiles(void) {
  core_string_t version = scan_lex_unicode_version();

  if (!version.data) {
    parse_grammar_t *g = unicode_identifier_grammar(nullptr);
    check("Unicode profiles fail clearly without a provider",
          g && !scan_grammar_lex_compiled(g) && scan_lex_build_error() &&
              strstr(scan_lex_build_error(), "ICU-enabled"));
    parse_grammar_free(g);
    return;
  }

  char version_text[32];
  snprintf(version_text, sizeof(version_text), "%.*s", (int)version.u8_bytes,
           version.data);
  parse_grammar_t *g = unicode_identifier_grammar(version_text);
  scan_lex_t *lex = g ? scan_grammar_lex_compiled(g) : nullptr;
  scan_lex_info_t info = {0};

  check("a Unicode identifier profile records its provider version",
        lex && scan_lex_inspect(lex, &info) &&
            core_string_eq(info.unicode_version, version));

  const char decomposed[] = "e\xCC\x81 ";
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts =
      scan_tokenize(g, decomposed, sizeof(decomposed) - 1, &buf, &sc);
  parse_token_info_t *tok = scan_stream_next(ts);
  core_string_t value = tok && core_option_is_set(tok->value)
                            ? core_option_get(tok->value)
                            : (core_string_t){0};
  scan_stream_next(ts);
  check("identifier normalization preserves original source spans",
        tok && tok->start_byte == 0 && tok->end_byte == 3 &&
            value.u8_bytes == 2 && !memcmp(value.data, "\xC3\xA9", 2) &&
            tok->owns_value && scan_status(sc) == SCAN_STATUS_COMPLETE);
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  ts = scan_tokenize_incremental_owned(g);
  bool resumed =
      scan_stream_append(ts, "e", 1) && scan_stream_next(ts) == nullptr &&
      scan_stream_append(ts, "\xCC", 1) && scan_stream_next(ts) == nullptr &&
      scan_stream_append(ts, "\x81 ", 2);
  tok = resumed ? scan_stream_next(ts) : nullptr;
  value = tok && core_option_is_set(tok->value) ? core_option_get(tok->value)
                                                : (core_string_t){0};
  check("Unicode identifiers resume across one-byte UTF-8 chunks",
        tok && value.u8_bytes == 2 && !memcmp(value.data, "\xC3\xA9", 2));
  scan_token_stream_free(ts);

  buf = nullptr;
  sc = nullptr;
  ts = scan_tokenize(g, "a\xCE\xB1", 3, &buf, &sc);
  scan_stream_next(ts);
  check("mixed-script diagnostics identify the introducing scalar",
        scan_status(sc) == SCAN_STATUS_INVALID && sc->status_start_byte == 1 &&
            sc->status_end_byte == 3);
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  buf = nullptr;
  sc = nullptr;
  ts = scan_tokenize(g, "\xD0\xB0", 2, &buf, &sc);
  scan_stream_next(ts);
  check("script restrictions report a narrow source span",
        scan_status(sc) == SCAN_STATUS_INVALID && sc->status_start_byte == 0 &&
            sc->status_end_byte == 2);
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  buf = nullptr;
  sc = nullptr;
  ts = scan_tokenize(g, "\xF0\x9F\x8E\x89", 4, &buf, &sc);
  tok = scan_stream_next(ts);
  check("emoji policy can diagnose an otherwise tokenized identifier",
        tok && scan_status(sc) == SCAN_STATUS_INVALID &&
            sc->status_start_byte == 0 && sc->status_end_byte == 4);
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);

  g = unicode_confusable_grammar();
  buf = nullptr;
  sc = nullptr;
  ts = scan_tokenize(g, "\xD0\xB0", 2, &buf, &sc);
  scan_stream_next(ts);
  check("confusable checking is an optional narrow diagnostic",
        scan_status(sc) == SCAN_STATUS_INVALID && sc->status_start_byte == 0 &&
            sc->status_end_byte == 2);
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);

  g = unicode_identifier_grammar("0.0");
  check("identifier profiles reject unavailable Unicode versions",
        g && !scan_grammar_lex_compiled(g) && scan_lex_build_error() &&
            strstr(scan_lex_build_error(), "requests version"));
  parse_grammar_free(g);

  static const char *const UNKNOWN_SCRIPT =
      "@lex-ident-profile(\"bad\", scripts(\"NotAScript\"))\n"
      "@lex-ident(as(\"IDENTIFIER\"), profile(\"bad\"))\n"
      "<file> ::= %IDENTIFIER\n";
  core_string_t bad_text = core_string_from_cstr(UNKNOWN_SCRIPT);
  g = parse_grammar_new();
  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, bad_text);
  bool loaded = parse_bnf_load(bad_text, CORE_STRING_STATIC("file"), g);
  check("identifier profiles reject unknown script names at construction",
        loaded && !scan_grammar_lex_compiled(g) && scan_lex_build_error() &&
            strstr(scan_lex_build_error(), "unknown script"));
  parse_grammar_free(g);
  core_free(bad_text.data);
}

typedef struct {
  parse_grammar_t *grammar;
  const char *source;
  size_t expected_tokens;
  core_string_t expected_token;
  core_string_t expected_value;
  core_string_t expected_type;
  bool ok;
} concurrent_lex_t;

static void *tokenize_concurrently(void *arg) {
  concurrent_lex_t *run = arg;

  run->ok = true;

  for (size_t i = 0; i < 200; i++) {
    core_buffer_t *buf = nullptr;
    parse_scanner_t *sc = nullptr;
    parse_token_stream_t *ts = scan_tokenize(run->grammar, run->source,
                                             strlen(run->source), &buf, &sc);
    size_t count = 0;

    parse_token_info_t *token = nullptr;
    while ((token = scan_stream_next(ts))) {
      count++;
      if ((run->expected_token.data &&
           token->tid != parse_terminal_id(run->grammar,
                                           run->expected_token)) ||
          (run->expected_value.data &&
           (!core_option_is_set(token->value) ||
            !core_string_eq(core_option_get(token->value),
                            run->expected_value))) ||
          (run->expected_type.data &&
           (!core_option_is_set(token->literal_type) ||
            !core_string_eq(core_option_get(token->literal_type),
                            run->expected_type)))) {
        run->ok = false;
      }
    }

    size_t expected = run->expected_tokens ? run->expected_tokens : 1;

    if (count != expected || scan_status(sc) != SCAN_STATUS_COMPLETE) {
      run->ok = false;
    }

    scan_token_stream_free(ts);
    scan_scanner_free(sc);
    core_buffer_free(buf);
  }

  return nullptr;
}

static void test_compiled_lexers_are_safe_to_share(void) {
  parse_grammar_t *g = fence_grammar();

  parse_grammar_finalize(g);
  check("grammar finalization compiles its immutable lexical program",
        g->lex_compiled != nullptr);

  concurrent_lex_t runs[] = {
      {.grammar = g, .source = "[=[alpha]=]"},
      {.grammar = g, .source = "[==ffi[beta]==]"},
      {.grammar = g, .source = "[===bnf[gamma]===]"},
      {.grammar = g, .source = "[=json[delta]=]"},
  };
  pthread_t threads[sizeof(runs) / sizeof(runs[0])];

  for (size_t i = 0; i < sizeof(runs) / sizeof(runs[0]); i++) {
    pthread_create(&threads[i], nullptr, tokenize_concurrently, &runs[i]);
  }

  bool ok = true;

  for (size_t i = 0; i < sizeof(runs) / sizeof(runs[0]); i++) {
    pthread_join(threads[i], nullptr);
    ok = ok && runs[i].ok;
  }

  check("one compiled lexical program serves concurrent tokenizations", ok);
  parse_grammar_free(g);

  parse_grammar_t *embedded = embedded_grammar("json", true);
  concurrent_lex_t embedded_runs[] = {
      {.grammar = embedded, .source = "a {{1 \"x\"}} b", .expected_tokens = 6},
      {.grammar = embedded, .source = "c {{2 \"y\"}} d", .expected_tokens = 6},
      {.grammar = embedded, .source = "e {{3 \"z\"}} f", .expected_tokens = 6},
      {.grammar = embedded, .source = "g {{4 \"w\"}} h", .expected_tokens = 6},
  };
  pthread_t embedded_threads[sizeof(embedded_runs) / sizeof(embedded_runs[0])];

  for (size_t i = 0; i < sizeof(embedded_runs) / sizeof(embedded_runs[0]);
       i++) {
    pthread_create(&embedded_threads[i], nullptr, tokenize_concurrently,
                   &embedded_runs[i]);
  }

  ok = true;
  for (size_t i = 0; i < sizeof(embedded_runs) / sizeof(embedded_runs[0]);
       i++) {
    pthread_join(embedded_threads[i], nullptr);
    ok = ok && embedded_runs[i].ok;
  }

  check("nested compiled lexers are immutable across concurrent sessions", ok);
  parse_grammar_free(embedded);

  scan_lex_literal_hook_register_pure(S("test-literal-hook"),
                                      test_literal_hook);
  literal_hook_calls = 0;
  parse_grammar_t *hooked = literal_hook_grammar();
  concurrent_lex_t hook_runs[] = {
      {.grammar = hooked,
       .source = "\"parallel\"",
       .expected_token = S("SPECIAL"),
       .expected_value = S("decoded"),
       .expected_type = S("duration")},
      {.grammar = hooked,
       .source = "\"parallel\"",
       .expected_token = S("SPECIAL"),
       .expected_value = S("decoded"),
       .expected_type = S("duration")},
      {.grammar = hooked,
       .source = "\"parallel\"",
       .expected_token = S("SPECIAL"),
       .expected_value = S("decoded"),
       .expected_type = S("duration")},
      {.grammar = hooked,
       .source = "\"parallel\"",
       .expected_token = S("SPECIAL"),
       .expected_value = S("decoded"),
       .expected_type = S("duration")},
  };
  pthread_t hook_threads[sizeof(hook_runs) / sizeof(hook_runs[0])];

  for (size_t i = 0; i < sizeof(hook_runs) / sizeof(hook_runs[0]); i++) {
    pthread_create(&hook_threads[i], nullptr, tokenize_concurrently,
                   &hook_runs[i]);
  }
  ok = true;
  for (size_t i = 0; i < sizeof(hook_runs) / sizeof(hook_runs[0]); i++) {
    pthread_join(hook_threads[i], nullptr);
    ok = ok && hook_runs[i].ok;
  }
  check("literal hook results are deterministic across parallel sessions",
        ok && atomic_load(&literal_hook_calls) >= 1 &&
            atomic_load(&literal_hook_calls) <=
                sizeof(hook_runs) / sizeof(hook_runs[0]));
  parse_grammar_free(hooked);

  if (scan_lex_unicode_version().data) {
    parse_grammar_t *unicode = unicode_confusable_grammar();
    concurrent_lex_t unicode_runs[] = {
        {.grammar = unicode, .source = "abc"},
        {.grammar = unicode, .source = "abc"},
        {.grammar = unicode, .source = "abc"},
        {.grammar = unicode, .source = "abc"},
    };
    pthread_t unicode_threads[sizeof(unicode_runs) / sizeof(unicode_runs[0])];

    for (size_t i = 0; i < sizeof(unicode_runs) / sizeof(unicode_runs[0]);
         i++) {
      pthread_create(&unicode_threads[i], nullptr, tokenize_concurrently,
                     &unicode_runs[i]);
    }
    ok = true;
    for (size_t i = 0; i < sizeof(unicode_runs) / sizeof(unicode_runs[0]);
         i++) {
      pthread_join(unicode_threads[i], nullptr);
      ok = ok && unicode_runs[i].ok;
    }
    check("Unicode profile providers are immutable across parallel sessions",
          ok);
    parse_grammar_free(unicode);
  }
}

static void test_lexer_spec_conflict_analysis(void) {
  family_error("cross-family duplicate delimiters are rejected",
               "@lex-comment(\"/\")\n"
               "@lex-string(\"/\", \"/\", as(\"STRING\"))\n"
               "<file> ::= %STRING\n",
               "shadows");
  family_error("an earlier short opener cannot hide a later family",
               "@lex-comment(\"/\")\n"
               "@lex-string(\"/*\", \"*/\", as(\"STRING\"))\n"
               "<file> ::= %STRING\n",
               "unreachable lexer declaration");
  static const char *const shadowed_source =
      "@lex-string(\"/\", \"/\", as(\"STRING\"))\n"
      "<file> ::= %STRING | %\"/\"\n";
  core_string_t shadowed_text = core_string_from_cstr(shadowed_source);
  parse_grammar_t *shadowed = parse_grammar_new();
  parse_grammar_set_error_recovery(shadowed, false);
  parse_bnf_declare_terminals(shadowed, shadowed_text);
  bool shadowed_loaded = parse_bnf_load(shadowed_text, S("file"), shadowed);
  scan_lex_t *shadowed_lex =
      shadowed_loaded ? scan_grammar_lex_compiled(shadowed) : nullptr;
  scan_lex_issue_t shadowed_issue = {0};
  bool found_shadow = false;
  for (size_t i = 0; shadowed_lex && i < scan_lex_issue_count(shadowed_lex);
       i++) {
    if (scan_lex_issue(shadowed_lex, i, &shadowed_issue) &&
        shadowed_issue.kind == SCAN_LEX_ISSUE_SHADOWED_TERMINAL &&
        core_string_eq(shadowed_issue.token, S("/"))) {
      found_shadow = true;
    }
  }
  check("shadowed grammar terminals are reported without rejection",
        found_shadow);
  parse_grammar_free(shadowed);
  core_free(shadowed_text.data);
  static const scan_lex_comment_t prefix_comments[] = {
      {.kind = SCAN_LEX_BLOCK, .opener = S("/*"), .closer = S("*/")},
      {},
  };
  static const scan_lex_regex_t prefix_regexes[] = {
      {.opener = S("/"), .closer = S("/"), .tok = S("REGEX")},
      {},
  };
  static const scan_lex_spec_t valid_prefixes = {
      .name = S("analysis-valid-prefixes"),
      .comments = prefix_comments,
      .regexes = prefix_regexes,
  };
  static const char *const prefix_terms[] = {"REGEX", nullptr};
  parse_grammar_t *g = mk(&valid_prefixes, prefix_terms);
  check("an earlier longer opener and later prefix remain reachable",
        scan_grammar_lex_compiled(g) != nullptr);
  parse_grammar_free(g);

  static const scan_lex_sigil_t sigils[] = {
      {.prefix = S("$"), .tok = S("SHORT")},
      {.prefix = S("$$"), .tok = S("LONG")},
      {},
  };
  static const scan_lex_spec_t longest = {
      .name = S("analysis-longest-prefix"),
      .sigils = sigils,
  };
  static const char *const sigil_terms[] = {"SHORT", "LONG", nullptr};
  g = mk(&longest, sigil_terms);
  kinds(g, "same-family prefixes choose the longest opener", "$$name $other",
        "LONG SHORT");
  parse_grammar_free(g);

  static const scan_lex_string_t unknown_strings[] = {
      {.opener = S("\""), .closer = S("\""), .tok = S("NOT_DECLARED")},
      {},
  };
  static const scan_lex_spec_t unknown_output = {
      .name = S("analysis-unknown-output"),
      .strings = unknown_strings,
  };
  g = parse_grammar_new();
  scan_lex_t *lex = scan_lex_compile(&unknown_output, g);
  check("explicit output names must belong to the grammar",
        !lex && scan_lex_compile_error() &&
            strstr(scan_lex_compile_error(), "unknown token 'NOT_DECLARED'"));
  parse_grammar_free(g);

  static const core_string_t duplicate_hook_outputs[] = {
      S("SPECIAL"),
      S("SPECIAL"),
      {},
  };
  static const scan_lex_literal_hook_t duplicate_hook[] = {
      {.token = S("STRING"),
       .hook = S("unused-hook"),
       .outputs = duplicate_hook_outputs},
      {},
  };
  static const scan_lex_spec_t duplicate_output = {
      .name = S("analysis-duplicate-output"),
      .literal_hooks = duplicate_hook,
  };
  g = parse_grammar_new();
  parse_register_terminal(g, S("SPECIAL"));
  lex = scan_lex_compile(&duplicate_output, g);
  check(
      "duplicate output declarations are rejected deterministically",
      !lex && scan_lex_compile_error() &&
          strstr(scan_lex_compile_error(), "duplicate output token 'SPECIAL'"));
  parse_grammar_free(g);

  static const core_string_t missing_operator[] = {S("missing-op"), {}};
  static const scan_lex_spec_t bad_operator = {
      .name = S("analysis-bad-operator"),
      .operators = missing_operator,
  };
  g = parse_grammar_new();
  lex = scan_lex_compile(&bad_operator, g);
  check("programmatic operators must name grammar terminals",
        !lex && scan_lex_compile_error() &&
            strstr(scan_lex_compile_error(), "unknown token"));
  parse_grammar_free(g);

  static const scan_lex_string_t duplicate_a[] = {
      {.opener = S("qq"), .closer = S("q"), .tok = S("STRING")},
      {.opener = S("qq"), .closer = S("q"), .tok = S("STRING")},
      {},
  };
  static const scan_lex_string_t duplicate_b[] = {
      {.opener = S("qq"), .closer = S("q"), .tok = S("STRING")},
      {.opener = S("qq"), .closer = S("q"), .tok = S("STRING")},
      {},
  };
  static const scan_lex_declaration_t duplicate_origins[] = {
      {.kind = SCAN_LEX_DECL_STRING, .spelling = S("qq"), .origin = S("z.lex")},
      {.kind = SCAN_LEX_DECL_STRING, .spelling = S("qq"), .origin = S("a.lex")},
      {},
  };
  scan_lex_spec_t first = {
      .name = S("analysis-deterministic-a"),
      .strings = duplicate_a,
      .declarations = duplicate_origins,
  };
  scan_lex_spec_t second = {
      .name = S("analysis-deterministic-b"),
      .strings = duplicate_b,
      .declarations = duplicate_origins,
  };
  g = parse_grammar_new();
  lex = scan_lex_compile(&first, g);
  char first_error[256] = {0};
  if (!lex && scan_lex_compile_error()) {
    snprintf(first_error, sizeof(first_error), "%s", scan_lex_compile_error());
  }
  lex = scan_lex_compile(&second, g);
  check("conflict diagnostics are independent of declaration order",
        !lex && scan_lex_compile_error() &&
            !strcmp(first_error, scan_lex_compile_error()) &&
            strstr(first_error, "a.lex") && strstr(first_error, "z.lex"));
  parse_grammar_free(g);

  static const scan_lex_string_t outputs[] = {
      {.opener = S("\""), .closer = S("\""), .tok = S("STRING")},
      {},
  };
  static const scan_lex_declaration_t output_origins[] = {
      {.kind = SCAN_LEX_DECL_STRING,
       .spelling = S("\""),
       .origin = S("literals.lex")},
      {},
  };
  static const scan_lex_spec_t output_spec = {
      .name = S("analysis-unused-output"),
      .strings = outputs,
      .tok_ident = S("IDENTIFIER"),
      .declarations = output_origins,
  };
  static const char *const output_terms[] = {
      "IDENTIFIER",
      "STRING",
      nullptr,
  };
  g = bare_grammar(output_terms);
  parse_nonterm_t *file = parse_nonterm(g, S("file"));
  parse_add_rule(g, file,
                 PARSE_TERMINAL(parse_terminal_id(g, S("IDENTIFIER"))));
  lex = scan_lex_compile(&output_spec, g);
  scan_lex_issue_t issue = {0};
  check("unused emitted tokens are reported without rejecting a lexer",
        lex && scan_lex_issue_count(lex) == 1 &&
            scan_lex_issue(lex, 0, &issue) &&
            issue.kind == SCAN_LEX_ISSUE_UNUSED_OUTPUT &&
            core_string_eq(issue.token, S("STRING")) &&
            core_string_eq(issue.origin, S("literals.lex")));

  core_string_t image = scan_lex_image_encode(lex);
  scan_lex_t *loaded = scan_lex_image_load(image, g);
  issue = (scan_lex_issue_t){0};
  check("generated scanners preserve construction-time analysis issues",
        loaded && scan_lex_issue_count(loaded) == 1 &&
            scan_lex_issue(loaded, 0, &issue) &&
            core_string_eq(issue.token, S("STRING")) &&
            core_string_eq(issue.origin, S("literals.lex")));
  scan_lex_free(loaded);
  core_free(image.data);
  scan_lex_free(lex);
  parse_grammar_free(g);

  enum { MANY = 8192 };
  scan_lex_sigil_t *many = core_alloc_array(scan_lex_sigil_t, MANY + 1);
  scan_lex_declaration_t *many_origins =
      core_alloc_array(scan_lex_declaration_t, MANY + 1);
  char (*spellings)[16] = core_alloc_size(MANY + 1, sizeof(*spellings));
  for (size_t i = 0; i < MANY; i++) {
    int bytes = snprintf(spellings[i], sizeof(spellings[i]), "p%08zu", i);
    core_string_t spelling = core_string_borrow(spellings[i], (size_t)bytes);
    many[i] = (scan_lex_sigil_t){
        .prefix = spelling,
        .tok = S("IDENTIFIER"),
    };
    many_origins[i] = (scan_lex_declaration_t){
        .kind = SCAN_LEX_DECL_SIGIL,
        .spelling = spelling,
        .origin = S("large.lex"),
    };
  }
  scan_lex_spec_t large = {
      .name = S("analysis-large"),
      .sigils = many,
      .declarations = many_origins,
  };
  g = parse_grammar_new();
  lex = scan_lex_compile(&large, g);
  check("large declaration sets complete conflict analysis", lex != nullptr);
  scan_lex_free(lex);
  parse_grammar_free(g);
  core_free(spellings);
  core_free(many_origins);
  core_free(many);
}

typedef struct {
  parse_grammar_t *grammar;
  const scan_lex_conformance_suite_t *suite;
  size_t lane;
  size_t lanes;
  bool ok;
} generated_concurrency_t;

static uint64_t conformance_hash_bytes(uint64_t hash, const void *data,
                                       size_t bytes) {
  const unsigned char *at = data;
  for (size_t i = 0; i < bytes; i++) {
    hash ^= at[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static uint64_t conformance_scan(parse_grammar_t *grammar, core_string_t source,
                                 bool recovery, scan_status_t *status,
                                 size_t *tokens) {
  parse_scanner_t *scanner = nullptr;
  parse_token_stream_t *stream =
      scan_tokenize(grammar, source.data, source.u8_bytes, nullptr, &scanner);
  if (!stream) {
    *status = SCAN_STATUS_INVALID;
    *tokens = 0;
    return 0;
  }
  scan_scanner_set_recovery(scanner, recovery);
  uint64_t hash = UINT64_C(1469598103934665603);
  *tokens = 0;
  parse_token_info_t *token = nullptr;
  while ((token = scan_stream_next(stream))) {
    hash = conformance_hash_bytes(hash, &token->tid, sizeof(token->tid));
    hash = conformance_hash_bytes(hash, &token->start_byte,
                                  sizeof(token->start_byte));
    hash =
        conformance_hash_bytes(hash, &token->end_byte, sizeof(token->end_byte));
    if (core_option_is_set(token->value)) {
      core_string_t value = core_option_get(token->value);
      hash = conformance_hash_bytes(hash, value.data, value.u8_bytes);
    }
    (*tokens)++;
  }
  *status = scan_status(scanner);
  scan_token_stream_free(stream);
  scan_scanner_free(scanner);
  return hash;
}

static bool run_generated_case(parse_grammar_t *grammar,
                               const scan_lex_conformance_case_t *item) {
  core_string_t input =
      core_string_borrow(item->source.data, item->input_bytes);
  if (item->kind == SCAN_LEX_CONFORMANCE_TRUNCATION) {
    parse_token_stream_t *stream = scan_tokenize_incremental_owned(grammar);
    bool ok = stream && scan_stream_append(stream, input.data, input.u8_bytes);
    while (ok && scan_stream_next(stream)) {
    }
    ok = ok && scan_status(stream->scanner) == item->expected_status;
    scan_token_stream_free(stream);
    return ok;
  }
  if (item->kind == SCAN_LEX_CONFORMANCE_INCREMENTAL_SPLIT) {
    scan_status_t batch_status = SCAN_STATUS_ACTIVE;
    size_t batch_tokens = 0;
    uint64_t batch_hash =
        conformance_scan(grammar, input, false, &batch_status, &batch_tokens);
    parse_token_stream_t *stream = scan_tokenize_incremental_owned(grammar);
    bool ok =
        stream && scan_stream_append(stream, input.data, item->split_byte);
    while (ok && scan_stream_next(stream)) {
    }
    ok = ok && scan_status(stream->scanner) == SCAN_STATUS_INCOMPLETE &&
         scan_stream_append(stream, input.data + item->split_byte,
                            input.u8_bytes - item->split_byte) &&
         scan_stream_finish(stream);
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t tokens = 0;
    parse_token_info_t *token = nullptr;
    while (ok && (token = scan_stream_next(stream))) {
      hash = conformance_hash_bytes(hash, &token->tid, sizeof(token->tid));
      hash = conformance_hash_bytes(hash, &token->start_byte,
                                    sizeof(token->start_byte));
      hash = conformance_hash_bytes(hash, &token->end_byte,
                                    sizeof(token->end_byte));
      if (core_option_is_set(token->value)) {
        core_string_t value = core_option_get(token->value);
        hash = conformance_hash_bytes(hash, value.data, value.u8_bytes);
      }
      tokens++;
    }
    ok = ok && batch_status == SCAN_STATUS_COMPLETE &&
         scan_status(stream->scanner) == SCAN_STATUS_COMPLETE &&
         tokens == batch_tokens && hash == batch_hash;
    scan_token_stream_free(stream);
    return ok;
  }

  scan_status_t status = SCAN_STATUS_ACTIVE;
  size_t tokens = 0;
  conformance_scan(grammar, input, item->recovery, &status, &tokens);
  return status == item->expected_status;
}

static void *run_generated_concurrently(void *argument) {
  generated_concurrency_t *run = argument;
  run->ok = true;
  size_t count = scan_lex_conformance_count(run->suite);
  for (size_t i = run->lane; run->ok && i < count; i += run->lanes) {
    scan_lex_conformance_case_t item = {0};
    if (!scan_lex_conformance_case(run->suite, i, &item) ||
        item.kind != SCAN_LEX_CONFORMANCE_CONCURRENT) {
      continue;
    }
    run->ok = run_generated_case(run->grammar, &item);
  }
  return nullptr;
}

static bool run_generated_suite(parse_grammar_t *grammar,
                                const scan_lex_conformance_suite_t *suite) {
  bool ok = true;
  size_t count = scan_lex_conformance_count(suite);
  for (size_t i = 0; ok && i < count; i++) {
    scan_lex_conformance_case_t item = {0};
    scan_lex_conformance_case(suite, i, &item);
    if (item.kind == SCAN_LEX_CONFORMANCE_CONCURRENT) {
      continue;
    }
    ok = run_generated_case(grammar, &item);
    if (!ok) {
      printf("generated lexer case %zu kind %u input %zu split %zu "
             "failed for '%.*s' in '%.*s'%s%.*s\n",
             item.id, (unsigned)item.kind, item.input_bytes, item.split_byte,
             (int)item.spelling.u8_bytes, item.spelling.data,
             (int)item.source.u8_bytes, item.source.data,
             item.origin.data ? " from " : "", (int)item.origin.u8_bytes,
             item.origin.data ? item.origin.data : "");
    }
  }

  enum { LANES = 4 };
  generated_concurrency_t runs[LANES];
  pthread_t threads[LANES];
  for (size_t i = 0; i < LANES; i++) {
    runs[i] = (generated_concurrency_t){
        .grammar = grammar,
        .suite = suite,
        .lane = i,
        .lanes = LANES,
    };
    pthread_create(&threads[i], nullptr, run_generated_concurrently, &runs[i]);
  }
  for (size_t i = 0; i < LANES; i++) {
    pthread_join(threads[i], nullptr);
    ok = ok && runs[i].ok;
  }
  return ok;
}

static void test_spec_driven_conformance_cases(void) {
  parse_grammar_t *grammars[] = {
      mk(&PYTHON, PY_TERMS),
      mk(&JS, (const char *const[]){"IDENTIFIER", "INTEGER", "FLOAT", "STRING",
                                    "TEMPLATE_HEAD", "TEMPLATE_MIDDLE",
                                    "TEMPLATE_TAIL", "REGEX_LIT", nullptr}),
      mk(&RUBY, (const char *const[]){"IDENTIFIER", "INTEGER", "HEREDOC",
                                      "CVAR", "IVAR", "GVAR", nullptr}),
      fence_grammar(),
      mk(&RECOVERY_LEXER,
         (const char *const[]){"IDENTIFIER", "INTEGER", "STRING", "ERROR", ";",
                               nullptr}),
  };
  bool ok = true;
  for (size_t i = 0; i < sizeof(grammars) / sizeof(grammars[0]); i++) {
    scan_lex_t *program = scan_grammar_lex_compiled(grammars[i]);
    scan_lex_conformance_suite_t *first =
        scan_lex_conformance_generate(program, 0);
    scan_lex_conformance_suite_t *second =
        scan_lex_conformance_generate(program, 0);
    ok = ok && first && second &&
         scan_lex_conformance_count(first) ==
             scan_lex_conformance_count(second) &&
         scan_lex_conformance_fingerprint(first) ==
             scan_lex_conformance_fingerprint(second) &&
         run_generated_suite(grammars[i], first);
    scan_lex_conformance_free(second);
    scan_lex_conformance_free(first);
  }
  check("resolved recipes generate reproducible passing conformance cases", ok);

  static const scan_lex_string_t boundary_strings[] = {
      {.opener = S("abc"), .closer = S("wxyz"), .tok = S("STRING")},
      {},
  };
  static const scan_lex_declaration_t boundary_origins[] = {
      {.kind = SCAN_LEX_DECL_STRING,
       .spelling = S("abc"),
       .origin = S("boundary.lex")},
      {},
  };
  static const scan_lex_spec_t boundary_spec = {
      .name = S("boundary-test"),
      .strings = boundary_strings,
      .declarations = boundary_origins,
  };
  parse_grammar_t *boundary =
      mk(&boundary_spec, (const char *const[]){"STRING", nullptr});
  scan_lex_t *program = scan_grammar_lex_compiled(boundary);
  scan_lex_conformance_suite_t *suite =
      scan_lex_conformance_generate(program, 0);
  bool splits[8] = {0};
  bool has_origin = false;
  size_t split_cases = 0;
  for (size_t i = 0; i < scan_lex_conformance_count(suite); i++) {
    scan_lex_conformance_case_t item = {0};
    scan_lex_conformance_case(suite, i, &item);
    if (item.kind == SCAN_LEX_CONFORMANCE_INCREMENTAL_SPLIT &&
        item.split_byte < sizeof(splits)) {
      splits[item.split_byte] = true;
      split_cases++;
      has_origin = has_origin || core_string_eq(item.origin, S("boundary.lex"));
    }
  }
  check("every opener and closer byte boundary is generated",
        split_cases == 6 && splits[1] && splits[2] && splits[3] && splits[5] &&
            splits[6] && splits[7]);
  check("generated cases identify their originating declaration", has_origin);

  static const core_string_t prefix_operators[] = {
      S("a"),
      S("ab"),
      S("ac"),
      {},
  };
  scan_lex_spec_t prefix_spec = {
      .name = S("prefix-conformance"),
      .operators = prefix_operators,
  };
  parse_grammar_t *prefix_grammar = parse_grammar_new();
  for (size_t i = 0; prefix_operators[i].data; i++) {
    parse_register_terminal(prefix_grammar, prefix_operators[i]);
  }
  scan_lex_t *prefix_program = scan_lex_compile(&prefix_spec, prefix_grammar);
  scan_lex_conformance_suite_t *prefix_suite =
      scan_lex_conformance_generate(prefix_program, 0);
  bool sibling_overlap = false;
  for (size_t i = 0; i < scan_lex_conformance_count(prefix_suite); i++) {
    scan_lex_conformance_case_t item = {0};
    scan_lex_conformance_case(prefix_suite, i, &item);
    sibling_overlap =
        sibling_overlap || (item.kind == SCAN_LEX_CONFORMANCE_LONGEST_PREFIX &&
                            core_string_eq(item.spelling, S("ac")));
  }
  check("prefix siblings all generate longest-match cases", sibling_overlap);
  scan_lex_conformance_free(prefix_suite);
  scan_lex_free(prefix_program);
  parse_grammar_free(prefix_grammar);

  core_string_t image = scan_lex_image_encode(program);
  scan_lex_t *loaded = scan_lex_image_load(image, boundary);
  scan_lex_conformance_suite_t *loaded_suite =
      scan_lex_conformance_generate(loaded, 0);
  check("generated scanners produce identical conformance cases",
        loaded_suite && scan_lex_conformance_fingerprint(suite) ==
                            scan_lex_conformance_fingerprint(loaded_suite));
  scan_lex_conformance_free(loaded_suite);
  scan_lex_free(loaded);
  core_free(image.data);
  scan_lex_conformance_free(suite);
  parse_grammar_free(boundary);

  enum { OPERATORS = 1024 };
  core_string_t *operators = core_alloc_array(core_string_t, OPERATORS + 1);
  char (*names)[16] = core_alloc_size(OPERATORS + 1, sizeof(*names));
  parse_grammar_t *large = parse_grammar_new();
  for (size_t i = 0; i < OPERATORS; i++) {
    int bytes = snprintf(names[i], sizeof(names[i]), "op%06zu", i);
    operators[i] = core_string_borrow(names[i], (size_t)bytes);
    parse_register_terminal(large, operators[i]);
  }
  scan_lex_spec_t large_spec = {
      .name = S("bounded-conformance"),
      .operators = operators,
  };
  program = scan_lex_compile(&large_spec, large);
  suite = scan_lex_conformance_generate(program, 64);
  check("large vocabularies obey the requested generated-case bound",
        suite && scan_lex_conformance_count(suite) == 64);
  scan_lex_conformance_free(suite);
  scan_lex_free(program);
  parse_grammar_free(large);
  core_free(names);
  core_free(operators);

  for (size_t i = 0; i < sizeof(grammars) / sizeof(grammars[0]); i++) {
    parse_grammar_free(grammars[i]);
  }
}

int main(void) {
  test_indentation_becomes_tokens();
  test_layout_tokens_ignore_only_when_unmatched();
  test_indentation_policies();
  test_incremental_indentation_waits_for_content();
  test_embedded_lexer_switching();
  test_interpolation_pushes_a_mode();
  test_templates_and_regex();
  test_sigils_and_heredocs();
  test_heredoc_families();
  test_literal_postprocessing_hooks();
  test_a_spec_is_reusable_across_runs();
  test_a_grammar_declares_its_own_lexis();
  test_string_families_expand_once();
  test_string_families_reject_duplicate_openers();
  test_string_families_diagnose_invalid_declarations();
  test_paired_string_families_nest();
  test_composable_string_escape_profiles();
  test_large_string_family_dispatch_is_bounded();
  test_large_number_suffix_dispatch_is_bounded();
  test_long_numeric_trie_paths_resume_linearly();
  test_an_unknown_option_is_named();
  test_lex_declarations_have_no_fixed_limit();
  test_zero_minimum_hash_fences();
  test_symmetric_fences_preserve_source_spans();
  test_fence_padding_requires_both_boundaries();
  test_unterminated_constructs_are_incomplete();
  test_incomplete_fences_resume_after_append();
  test_incomplete_strings_and_comments_resume();
  test_filled_block_comment_delimiters();
  test_interpolation_regex_and_heredoc_resume();
  test_incremental_stream_accepts_arbitrary_chunks();
  test_incremental_utf8_and_source_compaction();
  test_source_compaction_is_amortized();
  test_incremental_source_edits();
  test_incremental_scanning_is_linear();
  test_convenience_tokenization_owns_its_resources();
  test_generated_scanner_images();
  test_terminals_have_no_byte_width_limit();
  test_scanner_diagnostics_have_a_stable_view();
  test_grammar_finalization_is_thread_safe();
  test_numeric_recipes_reject_malformed_forms();
  test_malformed_token_recovery();
  test_self_contained_number_forms();
  test_number_forms_diagnose_invalid_declarations();
  test_reusable_lexical_packages();
  test_soft_keywords();
  test_strings_distinguish_incomplete_from_invalid();
  test_reject_fallback_accepts_declared_codepoints_only();
  test_unicode_identifier_profiles();
  test_lexer_spec_conflict_analysis();
  test_spec_driven_conformance_cases();
  test_compiled_lexers_are_safe_to_share();

  return report("scanner lex");
}
