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
#include "private/scanner/lex_decl.h"
#include "scanner/lex.h"
#include "scanner/token_stream.h"
#include "scanner/tokenizer.h"

#define S(x) CORE_STRING_STATIC(x)

#define HEX "0123456789abcdefABCDEF"

static size_t
content_start(const parse_token_info_t *token)
{
  size_t start = 0;
  parse_token_content_span(token, &start, nullptr);
  return start;
}

static size_t
content_end(const parse_token_info_t *token)
{
  size_t end = 0;
  parse_token_content_span(token, nullptr, &end);
  return end;
}

static bool
has_content(const parse_token_info_t *token)
{
  return parse_token_content_span(token, nullptr, nullptr);
}

static size_t
label_start(const parse_token_info_t *token)
{
  size_t start = 0;
  parse_token_label_span(token, &start, nullptr);
  return start;
}

static size_t
label_end(const parse_token_info_t *token)
{
  size_t end = 0;
  parse_token_label_span(token, nullptr, &end);
  return end;
}

static bool
has_label(const parse_token_info_t *token)
{
  return parse_token_label_span(token, nullptr, nullptr);
}

static uint32_t
layout_depth(const parse_token_info_t *token)
{
  uint32_t depth = 0;
  parse_token_layout(token, &depth, nullptr);
  return depth;
}

static uint32_t
layout_column(const parse_token_info_t *token)
{
  uint32_t column = 0;
  parse_token_layout(token, nullptr, &column);
  return column;
}

static const scan_lex_string_t JSON_STRINGS[] = {
    {.opener = S("\""), .closer = S("\""), .escape = S("\\"),
     .tok = S("STRING")},
    {},
};

static const scan_lex_number_form_t JSON_NUMBERS[] = {
    {.digits = S("0123456789"), .sign = S("-"), .exponent = S("eE"),
     .tok_int = S("INT_LIT"), .tok_float = S("FLOAT_LIT"),
     .flags = SCAN_LEX_NUM_POINT},
    {},
};

static const scan_lex_spec_t JSON = {
    .name = S("json"),
    .strings = JSON_STRINGS,
    .number_forms = JSON_NUMBERS,
    .tok_ident = S("IDENTIFIER"),
};

static scan_status_t source_status_of(parse_grammar_t *g,
                                      core_string_t source);
#define source_status(g, source) source_status_of((g), S(source))
static parse_grammar_t *unicode_identifier_grammar(core_string_t version);
static atomic_size_t literal_hook_calls;

static bool
load_test_grammar(core_string_t source, core_string_t start,
                  parse_grammar_t *grammar)
{
  if (source.u8_bytes < 5 || memcmp(source.data, "@lex-", 5)) {
    return parse_bnf_load(source, start, grammar);
  }

  size_t split = source.u8_bytes;
  for (size_t i = 0; i + 1 < source.u8_bytes; i++) {
    if (source.data[i] == '\n' && source.data[i + 1] == '<') {
      split = i + 1;
      break;
    }
  }

  core_string_t bnf = core_string_borrow(source.data + split,
                                         source.u8_bytes - split);
  if (bnf.u8_bytes && !parse_bnf_load(bnf, start, grammar)) {
    return false;
  }

  core_list_t(scan_lex_decl_t) decls = core_list_new(scan_lex_decl_t);
  bool ok = scan_lex_parse_file(core_string_borrow(source.data, split),
                                S("inline.lex"), &decls);
  if (ok) {
    const scan_lex_spec_t *spec = scan_lex_spec_from_decls(grammar, &decls);
    scan_grammar_set_lex(grammar, spec);
    grammar->lex_spec_owned = spec != nullptr;
  }
  scan_lex_decls_free(&decls);
  return ok;
}

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

static const scan_lex_number_form_t RECOVERY_NUMBERS[] = {
    {.digits = S("0123456789"), .separator = S("_"),
     .tok_int = S("INTEGER")},
    {},
};

static const scan_lex_spec_t RECOVERY_LEXER = {
    .name = S("recovery-test"),
    .strings = RECOVERY_STRINGS,
    .number_forms = RECOVERY_NUMBERS,
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
    .number_forms = RECOVERY_NUMBERS,
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
    .number_forms = RECOVERY_NUMBERS,
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

static const scan_lex_suffix_t PY_SUFFIXES[] = {
    {.suffix = S("j"), .tok = S("IMAGINARY")},
    {},
};

static const scan_lex_number_form_t PY_NUMBERS[] = {
    {.prefix = S("0x"), .digits = S(HEX), .tok_int = S("INTEGER")},
    {.prefix = S("0b"), .digits = S("01"), .tok_int = S("INTEGER")},
    {.digits = S("0123456789"), .separator = S("_"),
     .exponent = S("eE"), .tok_int = S("INTEGER"),
     .tok_float = S("FLOAT"), .suffixes = PY_SUFFIXES,
     .flags = SCAN_LEX_NUM_POINT | SCAN_LEX_NUM_LEADING_POINT},
    {},
};

static const scan_lex_spec_t PYTHON = {
    .name = S("lex-test-python"),
    .comments = PY_COMMENTS,
    .strings = PY_STRINGS,
    .number_forms = PY_NUMBERS,
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

static const scan_lex_number_form_t JS_NUMBERS[] = {
    {.prefix = S("0x"), .digits = S(HEX), .tok_int = S("INTEGER")},
    {.prefix = S("0b"), .digits = S("01"), .tok_int = S("INTEGER")},
    {.digits = S("0123456789"), .exponent = S("eE"),
     .tok_int = S("INTEGER"), .tok_float = S("FLOAT"),
     .flags = SCAN_LEX_NUM_POINT | SCAN_LEX_NUM_LEADING_POINT},
    {},
};

static const scan_lex_spec_t JS = {
    .name = S("lex-test-js"),
    .comments = JS_COMMENTS,
    .strings = JS_STRINGS,
    .regexes = JS_REGEXES,
    .number_forms = JS_NUMBERS,
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

static const scan_lex_number_form_t INTEGER_NUMBERS[] = {
    {.digits = S("0123456789"), .tok_int = S("INTEGER")},
    {},
};

static const scan_lex_spec_t RUBY = {
    .name = S("lex-test-ruby"),
    .heredocs = RB_HEREDOCS,
    .sigils = RB_SIGILS,
    .number_forms = INTEGER_NUMBERS,
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
                           const core_string_t *terms) {
  parse_grammar_t *g = parse_grammar_new();

  for (size_t i = 0; terms && terms[i].data; i++) {
    parse_register_terminal(g, terms[i]);
  }
  register_spec_outputs(g, spec);

  scan_lex_register(spec);
  scan_grammar_set_tokenizer(g, spec->name);

  return g;
}

static parse_grammar_t *bare_grammar(const core_string_t *terms) {
  parse_grammar_t *g = parse_grammar_new();

  for (size_t i = 0; terms && terms[i].data; i++) {
    parse_register_terminal(g, terms[i]);
  }
  return g;
}

static core_string_t render(parse_grammar_t *g, core_string_t src) {
  core_buffer_t *out = core_buffer_empty();
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;

  parse_token_stream_t *ts = scan_tokenize(g, src, &buf, &sc);

  if (!ts) {
    return core_buffer_take_string(out);
  }

  scan_stream_foreach(ts, tok) {
    core_string_t nm = {0};

    for (size_t i = 0; i < core_list_len(g->named_terms); i++) {
      if (g->named_terms.data[i].id == parse_token_tid(tok)) {
        nm = g->named_terms.data[i].value;

        break;
      }
    }

    if (out->byte_len) {
      core_buffer_putc(out, ' ');
    }
    if (nm.data) {
      core_buffer_append_string(out, nm);
    } else if (parse_token_tid(tok) > 0 && parse_token_tid(tok) < 128) {
      core_buffer_putc(out, (char)parse_token_tid(tok));
    } else {
      core_buffer_printf(out, "?%d", parse_token_tid(tok));
    }
  }

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  return core_buffer_take_string(out);
}

static void kinds_check(parse_grammar_t *g, core_string_t what,
                        core_string_t src, core_string_t want) {
  core_string_t got = render(g, src);

  if (!core_string_eq(got, want)) {
    fails++;
    printf("FAIL  %.*s\n   want: %.*s\n    got: %.*s\n", (int)what.u8_bytes,
           what.data, (int)want.u8_bytes, want.data, (int)got.u8_bytes,
           got.data);
  }
  core_free(got.data);
}

#define kinds(g, what, src, want) kinds_check((g), S(what), S(src), S(want))

static const core_string_t PY_TERMS[] = {
    S("NEWLINE"),        S("INDENT"),      S("DEDENT"), S("ENDMARKER"),
    S("IDENTIFIER"),     S("INTEGER"),     S("FLOAT"),  S("STRING"),
    S("IMAGINARY"),      S("FSTRING_START"), S("FSTRING_MIDDLE"),
    S("FSTRING_END"),    S("def"),         S("if"),     S("return"),
    {0},
};

#define indent_policy_grammar(policies) \
  indent_policy_grammar_text(S(policies))

static parse_grammar_t *indent_policy_grammar_text(core_string_t policies) {
  core_buffer_t *text = core_buffer_empty();

  core_buffer_printf(
      text,
      "@lex-comment(\"#\")\n"
      "@lex-layout(indent(\"INDENT\", \"DEDENT\"), "
      "newline(\"NEWLINE\"), eof(\"ENDMARKER\"), continue(\"\\\\\"), "
      "brackets(\"()[]{}\")%s%.*s)\n"
      "@lex-ident(as(\"IDENTIFIER\"))\n"
      "<file> ::= %%IDENTIFIER %%NEWLINE %%INDENT %%DEDENT %%ENDMARKER\n",
      policies.u8_bytes ? ", " : "", (int)policies.u8_bytes,
      policies.data ? policies.data : "");

  parse_grammar_t *g = parse_grammar_new();
  core_string_t source = core_string_borrow(text->data, text->byte_len);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, source);

  if (!load_test_grammar(source, CORE_STRING_STATIC("file"), g)) {
    core_string_t error = parse_bnf_error();
    fprintf(stderr, "indent policy grammar: %.*s\n", (int)error.u8_bytes,
            error.data);
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
  parse_token_stream_t *ts =
      scan_tokenize_owned(layout, S("a\n    b\n        c\nx\n"));
  parse_token_info_t *dedents[4] = {0};
  size_t dedent_count = 0;
  parse_token_info_t *tok;
  while ((tok = scan_stream_next(ts))) {
    if (parse_token_tid(tok) == parse_terminal_id(layout, S("DEDENT")) &&
        dedent_count < 4) {
      dedents[dedent_count++] = tok;
    }
  }
  check("each dedent records its resulting depth and column",
        dedent_count == 2 && parse_token_layout(dedents[0], nullptr, nullptr) &&
            layout_depth(dedents[0]) == 1 && layout_column(dedents[0]) == 4 &&
            layout_depth(dedents[1]) == 0 && layout_column(dedents[1]) == 0);
  scan_token_stream_free(ts);

  ts = scan_tokenize_owned(layout, S("a\n    b\n        c\n  x\n"));
  dedent_count = 0;
  while ((tok = scan_stream_next(ts))) {
    if (parse_token_tid(tok) == parse_terminal_id(layout, S("DEDENT")) &&
        dedent_count < 4) {
      dedents[dedent_count++] = tok;
    }
  }
  check("only the final dedent carries the unmatched-target marker",
        dedent_count == 2 && !parse_token_layout_mismatch(dedents[0]) &&
            parse_token_layout_mismatch(dedents[1]) &&
            layout_depth(dedents[1]) == 0 && layout_column(dedents[1]) == 0);
  scan_token_stream_free(ts);
  parse_grammar_free(layout);
}

static void test_newline_each_line(void) {
  scan_lex_spec_t spec = PYTHON;
  spec.newline_each_line = true;
  parse_grammar_t *g = mk(&spec, PY_TERMS);

  kinds(g, "blank and comment-only lines retain their newlines",
        "if x:\n\n  a\n# note\n\n  b\n",
        "if IDENTIFIER : NEWLINE NEWLINE INDENT IDENTIFIER NEWLINE "
        "NEWLINE NEWLINE IDENTIFIER NEWLINE DEDENT ENDMARKER");
  kinds(g, "bracketed newlines remain suppressed", "a = (1,\n\n  2)\n",
        "IDENTIFIER = ( INTEGER , INTEGER ) NEWLINE ENDMARKER");

  parse_grammar_free(g);
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
  parse_token_stream_t *ts = scan_tokenize_owned(tabs, S("a\n\tb\n"));

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
  ts = scan_tokenize_owned(tabs, S("a\tb"));
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
  ts = scan_tokenize_owned(mixed, S("a\n \tb\n"));

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
  ts = scan_tokenize_owned(unmatched, S("a\n  b\n c\n"));

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
  ts = scan_tokenize_owned(alignment, S("a = (\n  b)\n"));

  while (scan_stream_next(ts)) {
  }

  diagnostic = (scan_diagnostic_t){0};
  check("alignment policy composes with bracket suppression",
        scan_diagnostic(ts->scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_INVALID &&
            diagnostic.start_byte == 6 && diagnostic.end_byte == 8);
  scan_token_stream_free(ts);

  ts = scan_tokenize_owned(alignment, S("a \\\n  b\n"));

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
        bad && scan_grammar_lex(bad) == nullptr &&
            core_string_find(scan_lex_build_error(), S("tab_width")) >= 0);
  parse_grammar_free(bad);
}

static parse_grammar_t *layout_matching_grammar(bool colon_block) {
  core_string_t outside =
      S("@lex-layout(indent(\"INDENT\", \"DEDENT\"), "
        "newline(\"NEWLINE\"), eof(\"END\"))\n"
        "@lex-ident(as(\"IDENTIFIER\"))\n"
        "<file> ::= %IDENTIFIER %NEWLINE %IDENTIFIER %NEWLINE %END\n");
  core_string_t block =
      S("@lex-layout(indent(\"INDENT\", \"DEDENT\"), "
        "newline(\"NEWLINE\"), eof(\"END\"))\n"
        "@lex-ident(as(\"IDENTIFIER\"))\n"
        "<file> ::= %IDENTIFIER %\":\" %NEWLINE %INDENT %IDENTIFIER "
        "%NEWLINE %DEDENT %IDENTIFIER %NEWLINE %END\n");
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = colon_block ? block : outside;
  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = load_test_grammar(text, S("file"), g);
  if (!loaded) {
    core_string_t error = parse_bnf_error();
    fprintf(stderr, "layout grammar load failed: %.*s\n",
            (int)error.u8_bytes, error.data);
  }
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
  parse_token_stream_t *ts = scan_tokenize_owned(g, S("a\n  b\n"));
  parse_pwz_parser_t *parser = parse_pwz_new_for_start(
      g, parse_nonterm_id(parse_find_nonterm(g, S("file"))));
  bool parsed = parse_pwz_parse(parser, ts);
  check("unmatched ordinary indentation tokens do not block parsing", parsed);
  parse_pwz_free(parser);
  scan_token_stream_free(ts);
  parse_grammar_free(g);

  g = layout_matching_grammar(true);
  ts = scan_tokenize_owned(g, S("a:\n  b\nc\n"));
  parse_token_info_t *tok;
  parse_token_info_t *block_indent = nullptr;
  parse_token_info_t *block_dedent = nullptr;
  while ((tok = scan_stream_next(ts))) {
    if (parse_token_tid(tok) == parse_terminal_id(g, S("INDENT"))) {
      block_indent = tok;
    } else if (parse_token_tid(tok) == parse_terminal_id(g, S("DEDENT"))) {
      block_dedent = tok;
    }
  }
  parser = parse_pwz_new_for_start(
      g, parse_nonterm_id(parse_find_nonterm(g, S("file"))));
  check("a colon-newline block keeps both layout tokens significant",
        block_indent && block_dedent &&
            !parse_token_ignore_if_no_match(block_indent) &&
            !parse_token_ignore_if_no_match(block_dedent) &&
            parse_pwz_parse(parser, ts));
  parse_pwz_free(parser);
  scan_token_stream_free(ts);
  parse_grammar_free(g);

  g = layout_matching_grammar(true);
  core_string_t block_u2028 = S("a:\n  b\xE2\x80\xA8"
                                "c\n");
  ts = scan_tokenize_owned(g, block_u2028);
  kinds_check(g, S("U+2028 layout token sequence"), block_u2028,
              S("IDENTIFIER : NEWLINE INDENT IDENTIFIER NEWLINE DEDENT "
                "IDENTIFIER NEWLINE END"));
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
        scan_stream_append(ts, S("a\n  ")));
  check("tokens before an unfinished indentation prefix remain available",
        scan_stream_next(ts) != nullptr && scan_stream_next(ts) != nullptr);
  check("indentation waits without consuming its temporary boundary",
        scan_stream_next(ts) == nullptr &&
            scan_status(ts->scanner) == SCAN_STATUS_INCOMPLETE &&
            ts->scanner->cursor == 2);
  check("the completed indentation still emits its indent token",
        scan_stream_append(ts, S("b\n")) && scan_stream_next(ts) != nullptr);

  parse_token_info_t *b = scan_stream_next(ts);
  check("content follows the resumed indent token",
        b && parse_token_tid(b) == parse_terminal_id(g, S("IDENTIFIER")));

  scan_stream_finish(ts);
  while (scan_stream_next(ts)) {
  }
  check("the resumed indentation stream completes",
        scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);

  scan_token_stream_free(ts);

  ts = scan_tokenize_incremental_owned(g);
  check("a line-continuation prefix can end an incremental chunk",
        scan_stream_append(ts, S("a \\")) && scan_stream_next(ts) != nullptr &&
            scan_stream_next(ts) == nullptr &&
            scan_status(ts->scanner) == SCAN_STATUS_INCOMPLETE &&
            ts->scanner->cursor == 2);
  check("continued-line alignment resumes without indentation tokens",
        scan_stream_append(ts, S("\n  b\n")) &&
            scan_stream_next(ts) != nullptr);
  scan_stream_finish(ts);
  while (scan_stream_next(ts)) {
  }
  check("a split line continuation completes",
        scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);

  scan_token_stream_free(ts);
  parse_grammar_free(g);
}

static parse_grammar_t *embedded_grammar(core_string_t lexer, bool tokens) {
  core_buffer_t *text = core_buffer_empty();

  core_buffer_printf(
      text,
      "@lex-embed(\"{{\", \"}}\", lexer(\"%.*s\"), %s)\n"
      "@lex-ident(as(\"OUTER_ID\"))\n"
      "<file> ::= %%OUTER_ID | %%EMBED | %%EMBED_OPEN | %%EMBED_CLOSE | "
      "%%INNER_OPEN | %%INNER_CLOSE | %%MIDDLE_ID | %%IDENTIFIER | "
      "%%INT_LIT | %%FLOAT_LIT | %%STRING | %%A_OPEN | %%A_CLOSE | "
      "%%B_OPEN | %%B_CLOSE\n",
      (int)lexer.u8_bytes, lexer.data,
      tokens ? "tokens, open_as(\"EMBED_OPEN\"), close_as(\"EMBED_CLOSE\")"
             : "as(\"EMBED\")");

  parse_grammar_t *g = parse_grammar_new();
  core_string_t source = core_string_borrow(text->data, text->byte_len);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, source);

  if (!load_test_grammar(source, CORE_STRING_STATIC("file"), g)) {
    parse_grammar_free(g);
    g = nullptr;
  }

  core_buffer_free(text);
  return g;
}

static void embed_error(const char *label, core_string_t declaration,
                        core_string_t expected) {
  core_buffer_t *text = core_buffer_empty();
  core_buffer_append_string(text, declaration);
  core_buffer_puts(text, "\n<file> ::= %EMBED | %EMBED_OPEN | %EMBED_CLOSE\n");
  core_string_t source = core_string_borrow(text->data, text->byte_len);
  parse_grammar_t *g = parse_grammar_new();

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, source);
  bool loaded = load_test_grammar(source, CORE_STRING_STATIC("file"), g);
  scan_lex_t *compiled = loaded ? scan_grammar_lex_compiled(g) : nullptr;
  core_string_t error = scan_lex_build_error();

  check(label, loaded && !compiled && core_string_find(error, expected) >= 0);
  parse_grammar_free(g);
  core_buffer_free(text);
}

static void register_deep_embed_chain(void) {
  static bool initialized;
  static scan_lex_spec_t specs[80];
  static scan_lex_embed_t embeds[80][2];
  static core_string_t names[80];

  if (initialized) {
    return;
  }

  initialized = true;
  for (size_t i = 0; i < 80; i++) {
    core_buffer_t *name = core_buffer_empty();
    core_buffer_printf(name, "embed-depth-%zu", i);
    names[i] = core_buffer_take_string(name);
  }
  for (size_t i = 0; i < 80; i++) {
    specs[i].name = names[i];

    if (i + 1 < 80) {
      embeds[i][0] = (scan_lex_embed_t){
          .open = S("<"),
          .close = S(">"),
          .open_tok = S("EMBED_OPEN"),
          .close_tok = S("EMBED_CLOSE"),
          .tokens = true,
      };
      embeds[i][0].lexer = names[i + 1];
      specs[i].embeds = embeds[i];
    }
  }

  for (size_t i = 0; i < 80; i++) {
    scan_lex_register(&specs[i]);
  }
}

static void test_embedded_lexer_switching(void) {
  scan_lex_register(&NESTED);

  parse_grammar_t *tokens = embedded_grammar(S("json"), true);
  kinds(tokens, "an embedded lexer emits its own token sequence",
        "before {{1 \"x\"}} after",
        "OUTER_ID EMBED_OPEN INT_LIT STRING_LIT EMBED_CLOSE OUTER_ID");

  parse_token_stream_t *ts =
      scan_tokenize_owned(tokens, S("before {{1 \"x\"}} after"));
  parse_token_info_t *tok = nullptr;
  size_t count = 0;
  bool spans = true;

  while ((tok = scan_stream_next(ts))) {
    if (count == 1) {
      spans = parse_token_start_byte(tok) == 7
              && parse_token_end_byte(tok) == 9;
    } else if (count == 2) {
      spans = spans && parse_token_start_byte(tok) == 9
              && parse_token_end_byte(tok) == 10;
    } else if (count == 4) {
      spans = spans && parse_token_start_byte(tok) == 14
              && parse_token_end_byte(tok) == 16;
    }
    count++;
  }
  check("embedded tokens and delimiters retain absolute source spans",
        spans && count == 6);
  scan_token_stream_free(ts);

  ts = scan_tokenize_incremental_owned(tokens);
  count = 0;
  core_string_t source = S("before {{1 \"x\"}} after");

  for (size_t i = 0; i < source.u8_bytes; i++) {
    scan_stream_append(ts, core_string_slice(source, i, 1));
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

  ts = scan_tokenize_owned(tokens, S("x {{\"open}}\0"));
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

  parse_grammar_t *single = embedded_grammar(S("json"), false);
  ts = scan_tokenize_owned(single, S("x {{1}} y"));
  parse_token_info_t *outer = scan_stream_next(ts);
  parse_token_info_t *block = scan_stream_next(ts);

  check("single-token embedding includes delimiters and records its body",
        outer && block &&
            parse_token_tid(block) == parse_terminal_id(single, S("EMBED")) &&
            parse_token_start_byte(block) == 2 &&
            parse_token_end_byte(block) == 7 && has_content(block) &&
            content_start(block) == 4 && content_end(block) == 5);
  scan_token_stream_free(ts);

  ts = scan_tokenize_owned(single, S("x {{\"open}} y"));
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

  parse_grammar_t *nested = embedded_grammar(S("nested-embed-test"), true);

  kinds(nested, "embedded lexers can nest without mutable shared state",
        "a {{middle [1] tail}} z",
        "OUTER_ID EMBED_OPEN MIDDLE_ID INNER_OPEN INT_LIT INNER_CLOSE "
        "MIDDLE_ID EMBED_CLOSE OUTER_ID");
  parse_grammar_free(nested);

  scan_lex_register(&CYCLE_A);
  scan_lex_register(&CYCLE_B);
  parse_grammar_t *cycle = embedded_grammar(S("embed-cycle-a"), true);
  scan_lex_t *compiled = scan_grammar_lex_compiled(cycle);

  check("embedded lexer cycles are rejected during construction",
        !compiled &&
            core_string_find(scan_lex_compile_error(), S("cycle")) >= 0);
  parse_grammar_free(cycle);

  register_deep_embed_chain();
  parse_grammar_t *deep = embedded_grammar(S("embed-depth-0"), true);
  compiled = scan_grammar_lex_compiled(deep);
  check("embedded lexer nesting is not limited to 64 levels",
        compiled != nullptr);
  parse_grammar_free(deep);

  embed_error("delegated embeds name unknown lexers",
              S("@lex-embed(\"{{\", \"}}\", lexer(\"missing\"), "
                "as(\"EMBED\"))"),
              S("unknown lexer"));
  embed_error("token-sequence embeds require explicit delimiter tokens",
              S("@lex-embed(\"{{\", \"}}\", lexer(\"json\"), tokens)"),
              S("open_as"));

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
  parse_token_stream_t *ts =
      scan_tokenize(g, S("\"\"\"a\nb\"\"\""), &buf, &sc);
  parse_token_info_t *string = ts ? scan_stream_next(ts) : nullptr;

  check("a multiline token records both endpoint lines",
        string && parse_token_line(string) == 1 &&
            parse_token_column(string) == 1 &&
            parse_token_endline(string) == 2 &&
            parse_token_endcol(string) == 5);

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  parse_grammar_free(g);
}

static void test_templates_and_regex(void) {
  static const core_string_t T[] = {
      S("IDENTIFIER"),      S("INTEGER"),       S("STRING"),
      S("TEMPLATE_HEAD"),   S("TEMPLATE_MIDDLE"), S("TEMPLATE_TAIL"),
      S("REGEX_LIT"),       S("true"),          {0},
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
  static const core_string_t T[] = {
      S("IDENTIFIER"), S("INTEGER"), S("HEREDOC"), S("CVAR"),
      S("IVAR"),       S("GVAR"),    {0},
  };

  parse_grammar_t *g = mk(&RUBY, T);

  kinds(g, "the longest sigil wins", "@@a @b $c", "CVAR IVAR GVAR");
  kinds(g, "a bare sigil is not a sigil token", "@ 1", "@ INTEGER");

  kinds(g, "a heredoc body is one token and the line continues",
        "x = <<~EOS\n  body\nEOS\ny\n", "IDENTIFIER = HEREDOC IDENTIFIER");

  parse_grammar_free(g);
}

static void test_attached_postfix_sigil(void) {
  static const scan_lex_string_t strings[] = {
      {.opener = S("'"), .closer = S("'"), .tok = S("CHAR")},
      {.opener = S("\""), .closer = S("\""), .tok = S("STRING")},
      {},
  };
  static const scan_lex_sigil_t sigils[] = {
      {.prefix = S("'"), .before_string = true, .unless_closed = true,
       .attach_previous = true},
      {},
  };
  static const scan_lex_spec_t spec = {
      .name = S("postfix-sigil-test"),
      .strings = strings,
      .sigils = sigils,
      .tok_ident = S("IDENTIFIER"),
      .number_forms = INTEGER_NUMBERS,
  };
  static const core_string_t terms[] = {
      S("["),    S("]"),      S("INTEGER"), S("CHAR"),
      S("STRING"), S("IDENTIFIER"), {0},
  };
  parse_grammar_t *g = mk(&spec, terms);
  core_string_t source = S("[1]'array \"x\"'rich 'A' value[0]");
  parse_scanner_t *scanner = nullptr;
  parse_token_stream_t *stream = scan_tokenize(g, source, nullptr, &scanner);
  parse_token_info_t *tokens[16] = {0};
  size_t count = 0;
  tokens[count++] = scan_stream_next(stream);
  tokens[count++] = scan_stream_next(stream);
  tokens[count++] = scan_stream_next(stream);
  check("closing token is finalized before delivery",
        tokens[2] && core_option_is_set(parse_token_postfix_modifier(tokens[2]))
        && core_string_eq(core_option_get(parse_token_postfix_modifier(tokens[2])),
                          S("array")));
  for (parse_token_info_t *token; count < 16
       && (token = scan_stream_next(stream));)
    tokens[count++] = token;
  check("postfix sigils attach without emitting tokens", count == 9);
  check("closing delimiter carries its modifier",
        count > 2 && core_option_is_set(parse_token_postfix_modifier(tokens[2]))
        && core_string_eq(core_option_get(parse_token_postfix_modifier(tokens[2])),
                          S("array")));
  check("string carries its modifier",
        count > 3 && core_option_is_set(parse_token_postfix_modifier(tokens[3]))
        && core_string_eq(core_option_get(parse_token_postfix_modifier(tokens[3])),
                          S("rich")));
  check("closed character literal stays a character token",
        count > 4 && parse_token_tid(tokens[4]) == parse_terminal_id(g, S("CHAR")));
  check("ordinary indexing does not acquire a modifier",
        count > 8 && !core_option_is_set(parse_token_postfix_modifier(tokens[8])));
  scan_token_stream_free(stream);
  scan_scanner_free(scanner);
  parse_grammar_free(g);
}

static void check_conditional_sigil(parse_grammar_t *g) {
  kinds(g, "apostrophe separates type variables and character literals",
        "['T] 'a'", "[ TYPE_VAR ] CHAR_LIT");
  kinds(g, "escaped character literals retain their quotes",
        "'\\n' '\\''", "CHAR_LIT CHAR_LIT");
  kinds(g, "literal modifiers attach after eligible tokens",
        "\"x\"'rich []'array", "STRING_LIT [ ]");

  parse_scanner_t *scanner = nullptr;
  parse_token_stream_t *stream = scan_tokenize(
      g, S("['T] 'a' \"x\"'rich []'array"), nullptr, &scanner);
  parse_token_info_t *tokens[7] = {0};
  for (size_t i = 0; i < 7; i++) tokens[i] = scan_stream_next(stream);
  check("type-variable token retains its apostrophe",
        tokens[1] && parse_token_tid(tokens[1]) ==
            parse_terminal_id(g, S("TYPE_VAR"))
        && core_option_is_set(parse_token_value(tokens[1]))
        && core_string_eq(core_option_get(parse_token_value(tokens[1])),
                          S("'T")));
  check("adjacent string modifier remains attached",
        tokens[4] && core_option_is_set(parse_token_postfix_modifier(tokens[4]))
        && core_string_eq(core_option_get(
            parse_token_postfix_modifier(tokens[4])), S("rich")));
  check("adjacent list modifier remains attached",
        tokens[6] && core_option_is_set(parse_token_postfix_modifier(tokens[6]))
        && core_string_eq(core_option_get(
            parse_token_postfix_modifier(tokens[6])), S("array")));
  scan_token_stream_free(stream);
  scan_scanner_free(scanner);
}

static void test_conditional_attached_sigil(void) {
  static const scan_lex_string_t strings[] = {
      {.opener = S("'"), .closer = S("'"), .escape = S("\\"),
       .tok = S("CHAR_LIT")},
      {.opener = S("\""), .closer = S("\""), .tok = S("STRING_LIT")},
      {},
  };
  static const core_string_t attach_after[] = {
      S("]"), S("STRING_LIT"), {0},
  };
  static const scan_lex_sigil_t sigils[] = {
      {.prefix = S("'"), .tok = S("TYPE_VAR"), .before_string = true,
       .unless_closed = true, .attach_after = attach_after},
      {},
  };
  static const scan_lex_spec_t spec = {
      .name = S("conditional-sigil-test"),
      .strings = strings,
      .sigils = sigils,
      .tok_ident = S("IDENTIFIER"),
  };
  static const core_string_t terms[] = {
      S("["), S("]"), S("CHAR_LIT"), S("STRING_LIT"),
      S("TYPE_VAR"), S("IDENTIFIER"), {0},
  };
  parse_grammar_t *g = mk(&spec, terms);
  check_conditional_sigil(g);
  parse_grammar_free(g);

  core_string_t source = S(
      "@lex-string(\"'\", \"'\", escape(\"\\\\\"), scalar, "
      "as(\"CHAR_LIT\"))\n"
      "@lex-string(\"\\\"\", \"\\\"\", as(\"STRING_LIT\"))\n"
      "@lex-sigil(\"'\", before_string, unless_closed, as(\"TYPE_VAR\"), "
      "attach_after(\"]\", \"STRING_LIT\"))\n"
      "@lex-ident(as(\"IDENTIFIER\"))\n"
      "<file> ::= %\"[\" | %\"]\" | %CHAR_LIT | %STRING_LIT | "
      "%TYPE_VAR | %IDENTIFIER\n");
  g = parse_grammar_new();
  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, source);
  check("declarative conditional sigil loads",
        load_test_grammar(source, S("file"), g));
  check_conditional_sigil(g);
  parse_grammar_free(g);
}

static void test_heredoc_families(void) {
  core_string_t source =
      S("@lex-heredoc-header(\"shell-word\", quotes(\"single\"), "
        "tag_start(\"_\"), tag_continue(\"_-\"))\n"
        "@lex-heredoc(\"<<\", header(\"shell-word\"), queue, "
        "interpolate(\"unquoted\", \"double\"), "
        "interpolated_as(\"INTERPOLATED\"), as(\"RAW\"), "
        "terminator_indent(\"allow\"), body_indent(\"strip\"))\n"
        "@lex-ident(as(\"IDENTIFIER\"))\n"
        "<file> ::= %IDENTIFIER | %INTERPOLATED | %RAW\n");
  parse_grammar_t *g = parse_grammar_new();

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, source);
  check("named heredoc header profiles load",
        load_test_grammar(source, S("file"), g));

  core_string_t input = S("call <<A <<'B'\n"
                          "  one\n"
                          "  A\n"
                          "    two\n"
                          "  B\n"
                          "after");
  parse_token_stream_t *ts = scan_tokenize_owned(g, input);
  parse_token_info_t *call = scan_stream_next(ts);
  parse_token_info_t *first = scan_stream_next(ts);
  parse_token_info_t *second = scan_stream_next(ts);
  parse_token_info_t *after = scan_stream_next(ts);

  check("queued heredocs retain source order",
        call && first && second && after && !scan_stream_next(ts) &&
            parse_token_tid(first) == parse_terminal_id(g, S("INTERPOLATED")) &&
            parse_token_tid(second) == parse_terminal_id(g, S("RAW")));
  check("queued bodies receive quote-sensitive policies",
        parse_token_has_value(first) &&
            core_string_eq(core_option_get(parse_token_value(first)), S("one\n")) &&
            parse_token_has_value(second) &&
            core_string_eq(core_option_get(parse_token_value(second)), S("two\n")));
  check("heredoc tokens retain body and label spans",
        has_content(first) && has_label(first) &&
            has_content(second) && has_label(second));
  check("queued heredoc scanning resumes with following source",
        parse_token_has_value(after) &&
            core_string_eq(core_option_get(parse_token_value(after)), S("after")) &&
            scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);

  scan_token_stream_free(ts);

  ts = scan_tokenize_incremental_owned(g);
  size_t token_count = 0;
  for (size_t i = 0; i < input.u8_bytes; i++) {
    check("queued heredoc input accepts a one-byte append",
          scan_stream_append(ts, core_string_slice(input, i, 1)));
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
            scan_work_bytes(ts->scanner) < input.u8_bytes * 5);
  scan_token_stream_free(ts);

  core_string_t unicode_input = S("x <<λ\nbody\nλ\n");
  ts = scan_tokenize_incremental_owned(g);
  token_count = 0;
  for (size_t i = 0; i < unicode_input.u8_bytes; i++) {
    check("a Unicode heredoc tag accepts split UTF-8 input",
          scan_stream_append(ts, core_string_slice(unicode_input, i, 1)));
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
        scan_stream_append(ts, S("x <<A <<B\n")) &&
            scan_stream_next(ts) && scan_stream_next(ts) &&
            scan_stream_next(ts));
  check("discarding an earlier queued token does not retain a raw pointer",
        scan_stream_discard_consumed(ts));
  check("discarded queued tokens do not impede later body scanning",
        scan_stream_append(ts, S("one\nA\ntwo\nB\n")));
  scan_stream_finish(ts);
  while (scan_stream_next(ts)) {
  }
  check("queued scanning completes after consumed-token compaction",
        scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);
  scan_token_stream_free(ts);

  ts = scan_tokenize_owned(g, S("x <<\"BAD\"\nBAD\n"));
  while (scan_stream_next(ts)) {
  }
  scan_diagnostic_t diagnostic = {0};
  check("disallowed heredoc quote forms are malformed headers",
        scan_diagnostic(ts->scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_INVALID &&
            diagnostic.problem.kind
                == SCAN_DIAG_HEREDOC_TAG_QUOTED_NOT_PERMITTED);
  scan_token_stream_free(ts);

  ts = scan_tokenize_owned(g, S("x <<A\nbody\n"));
  while (scan_stream_next(ts)) {
  }
  diagnostic = (scan_diagnostic_t){0};
  check("missing queued bodies remain distinguishably incomplete",
        scan_diagnostic(ts->scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_INCOMPLETE &&
            diagnostic.problem.kind == SCAN_DIAG_HEREDOC_QUEUED_UNTERMINATED);
  scan_token_stream_free(ts);

  parse_grammar_free(g);
}

static parse_grammar_t *literal_hook_grammar(void) {
  core_string_t source =
      S("@lex-string('\"', '\"', as(\"STRING\"))\n"
        "@lex-literal-hook(\"STRING\", hook(\"test-literal-hook\"), "
        "modifier(\"unit\"), outputs(\"SPECIAL\"), "
        "types(\"duration\"))\n"
        "<file> ::= %STRING | %SPECIAL\n");
  parse_grammar_t *g = parse_grammar_new();

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, source);
  bool loaded = load_test_grammar(source, S("file"), g);

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
      scan_tokenize_owned(g, S("\"ok\" \"ok\""));
  parse_token_info_t *first = scan_stream_next(ts);
  parse_token_info_t *second = scan_stream_next(ts);

  check("a hook may decode a value and select a declared token",
        first && second && !scan_stream_next(ts) &&
            parse_token_tid(first) == parse_terminal_id(g, S("SPECIAL")) &&
            parse_token_tid(second) == parse_terminal_id(g, S("SPECIAL")) &&
            parse_token_has_value(first) &&
            core_string_eq(core_option_get(parse_token_value(first)), S("decoded")));
  check(
      "a hook may attach owned declared type metadata",
      core_option_is_set(parse_token_literal_type(first)) &&
          core_string_eq(core_option_get(parse_token_literal_type(first)), S("duration")));
  check("identical cacheable hook inputs execute once",
        literal_hook_calls == 1);
  scan_token_stream_free(ts);

  ts = scan_tokenize_owned(g, S("\"bad\""));
  while (scan_stream_next(ts)) {
  }
  scan_diagnostic_t diagnostic = {0};
  check("hook failures retain a relative narrow diagnostic span",
        scan_diagnostic(ts->scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_INVALID &&
            diagnostic.start_byte == 1 && diagnostic.end_byte == 4 &&
            diagnostic.problem.kind == SCAN_DIAG_HOOK_MESSAGE &&
            core_string_eq(diagnostic.problem.detail, S("bad literal")));
  scan_token_stream_free(ts);

  ts = scan_tokenize_owned(g, S("\"wrong\""));
  while (scan_stream_next(ts)) {
  }
  diagnostic = (scan_diagnostic_t){0};
  check("hooks cannot select undeclared token metadata",
        scan_diagnostic(ts->scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_INVALID &&
            diagnostic.problem.kind == SCAN_DIAG_HOOK_UNDECLARED_TOKEN);
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
  ts = scan_tokenize_owned(plain, S("123 \"plain\""));
  while (scan_stream_next(ts)) {
  }
  check("lexers without hook bindings pay no callback cost",
        literal_hook_calls == calls_before_plain_scan);
  scan_token_stream_free(ts);
  parse_grammar_free(plain);

  core_string_t unknown_source =
      S("@lex-string('\"', '\"', as(\"STRING\"))\n"
        "@lex-literal-hook(\"STRING\", hook(\"missing-hook\"))\n"
        "<file> ::= %STRING\n");
  parse_grammar_t *unknown = parse_grammar_new();
  parse_grammar_set_error_recovery(unknown, false);
  parse_bnf_declare_terminals(unknown, unknown_source);
  bool loaded = load_test_grammar(unknown_source, S("file"), unknown);
  check("unknown literal hooks fail during lexer construction",
        loaded && !scan_grammar_lex_compiled(unknown) &&
            core_string_find(scan_lex_compile_error(), S("unknown hook")) >= 0);
  parse_grammar_free(unknown);
}

static void test_a_grammar_declares_its_own_lexis(void) {
  static const char G[] =
      "@lex-comment(\"#\")\n"
      "@lex-comment(\"/*\", \"*/\", nested)\n"
      "@lex-string('\"', '\"', escape('\\\\'), as(\"STRING\"))\n"
      "@lex-string('f\"', '\"', escape('\\\\'), interp(\"{\", \"}\"), "
      "hole_tokens, "
      "as(\"FSTRING_START\", \"FSTRING_MIDDLE\", \"FSTRING_END\"))\n"
      "@lex-number(prefix(\"0x\"), digits(\"0123456789abcdef\"), "
      "int(\"INTEGER\"))\n"
      "@lex-number(digits(\"0123456789\"), sep(\"_\"), "
      "exp(\"eE\"), int(\"INTEGER\"), float(\"FLOAT\"), "
      "suffix(\"j\", \"IMAGINARY\"))\n"
      "@lex-layout(indent(\"INDENT\", \"DEDENT\"), newline(\"NEWLINE\"), "
      "eof(\"ENDMARKER\"), continue(\"\\\\\"), brackets(\"()[]{}\"))\n"
      "@lex-ident(as(\"IDENTIFIER\"))\n"
      "<file> ::= %IDENTIFIER %NEWLINE %INDENT %DEDENT %ENDMARKER "
      "%INTEGER %FLOAT %STRING %IMAGINARY %FSTRING_START "
      "%FSTRING_MIDDLE %FSTRING_END %\"def\" %\"return\"\n";

  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_borrow(G, sizeof(G) - 1);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);

  if (!load_test_grammar(text, CORE_STRING_STATIC("file"), g)) {
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

  check("nothing was rejected", scan_lex_build_error().data == nullptr);

  parse_grammar_free(g);
}

static parse_grammar_t *region_grammar(void) {
  static const char G[] =
      "@lex-region(\"#[\", \"#]\", \"#\")\n"
      "@lex-comment(\"#\")\n"
      "@lex-ident(as(\"IDENTIFIER\"))\n"
      "<file> ::= %IDENTIFIER*\n";
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_borrow(G, sizeof(G) - 1);
  parse_bnf_declare_terminals(g, text);
  bool loaded = load_test_grammar(text, S("region.lex"), g);
  if (!loaded) {
    core_string_t error = parse_bnf_error();
    fprintf(stderr, "region grammar load failed: %.*s\n",
            (int)error.u8_bytes, error.data);
  }
  if (!loaded) {
    parse_grammar_free(g);
    return nullptr;
  }
  return g;
}

static void test_comment_region_anchors_are_trivia(void) {
  parse_grammar_t *g = region_grammar();
  check("a comment-region lexer declaration loads", g != nullptr);
  if (!g) return;

  core_buffer_t *buffer = nullptr;
  parse_scanner_t *scanner = nullptr;
  parse_token_stream_t *stream = scan_tokenize(
      g, S("#[outer#\n# note\n#]outer#\nname"), &buffer, &scanner);
  parse_token_info_t *name = scan_stream_next(stream);
  while (scan_stream_next(stream)) {
  }
  parse_trivia_t *open = name ? parse_token_leading_trivia(name) : nullptr;
  parse_trivia_t *comment = open ? open->next : nullptr;
  parse_trivia_t *close = comment ? comment->next : nullptr;
  check("region anchors remain trivia ahead of ordinary comments",
        name && open && open->kind == PARSE_TRIVIA_REGION_OPEN
            && comment && comment->kind == PARSE_TRIVIA_COMMENT
            && close && close->kind == PARSE_TRIVIA_REGION_CLOSE);
  check("region anchor names and source spans are retained",
        open && close && core_string_eq(open->region_name, S("outer"))
            && core_string_eq(close->region_name, S("outer"))
            && open->start_byte == 0 && open->end_byte == 8
            && close->start_byte == 16 && close->end_byte == 24);
  check("ordinary comment trivia also retains its source span",
        comment && comment->start_byte == 9 && comment->end_byte == 15);
  check("matched anchors receive one stable region identity",
        open && close && open->region_id != 0
            && open->region_id == close->region_id);
  check("comments expose membership in every containing region",
        comment && parse_trivia_region_count(comment) == 1
            && parse_trivia_in_region(comment, open->region_id));
  parse_token_info_t *tree_token = parse_token_copy(name, true);
  parse_trivia_t *tree_comment = parse_token_leading_trivia(tree_token)->next;
  check("parse-tree token clones preserve comment-region metadata",
        tree_comment && parse_trivia_in_region(tree_comment, open->region_id));
  parse_token_free(tree_token);
  scan_token_stream_free(stream);
  scan_scanner_free(scanner);
  core_buffer_free(buffer);

  check("code before an anchor is rejected",
        source_status(g, "name #[#\n") == SCAN_STATUS_INVALID);
  check("code after an anchor is rejected",
        source_status(g, "#[# extra\n") == SCAN_STATUS_INVALID);
  check("an unterminated anchor is rejected",
        source_status(g, "#[name\n") == SCAN_STATUS_INVALID);
  check("an unmatched named close is rejected",
        source_status(g, "#]missing#\nname") == SCAN_STATUS_INVALID);
  check("an open region at EOF is rejected",
        source_status(g, "#[open#\nname") == SCAN_STATUS_INVALID);

  stream = scan_tokenize_owned(
      g, S("#[a#\n#[b#\n# both\n#]a#\n# b-only\n#]b#\nname"));
  while (scan_stream_next(stream)) {
  }
  name = scan_stream_get(stream, 0);
  open = parse_token_leading_trivia(name);
  parse_trivia_t *open_b = open->next;
  comment = open_b->next;
  parse_trivia_t *close_a = comment->next;
  parse_trivia_t *comment_b = close_a->next;
  parse_trivia_t *close_b = comment_b->next;
  check("named comment regions may cross and match by name",
        scan_status(stream->scanner) == SCAN_STATUS_COMPLETE
            && open->region_id == close_a->region_id
            && open_b->region_id == close_b->region_id
            && open->region_id != open_b->region_id);
  check("crossing-region comments retain exact memberships",
        parse_trivia_region_count(comment) == 2
            && parse_trivia_in_region(comment, open->region_id)
            && parse_trivia_in_region(comment, open_b->region_id)
            && parse_trivia_region_count(comment_b) == 1
            && parse_trivia_in_region(comment_b, open_b->region_id));
  scan_token_stream_free(stream);

  check("comment-region names in NFC match",
        source_status(g, "#[\xC3\xA9#\n# note\n#]\xC3\xA9#\nname") ==
            SCAN_STATUS_COMPLETE);
  check("comment-region names not in NFC are rejected",
        source_status(g, "#[e\xCC\x81#\n# note\n#]e\xCC\x81#\nname") ==
            SCAN_STATUS_INVALID);

  stream = scan_tokenize_owned(
      g, S("# before\n#[r#\n# content\n#]r#\n# after\n\nname"));
  while (scan_stream_next(stream)) {
  }
  name = scan_stream_get(stream, 0);
  parse_trivia_t *before = parse_token_leading_trivia(name);
  open = before->next;
  comment = open->next;
  close = comment->next;
  parse_trivia_t *after = close->next;
  check("comment-only lines group with anchors across blank lines",
        before->anchor_group == open->anchor_group
            && comment->anchor_group == open->anchor_group
            && after->anchor_group == close->anchor_group
            && open->anchor_group != close->anchor_group);
  check("anchor grouping does not expand region membership",
        parse_trivia_region_count(before) == 0
            && parse_trivia_region_count(comment) == 1
            && parse_trivia_region_count(after) == 0);
  scan_token_stream_free(stream);

  check("U+2028 separates standalone comment-region anchors",
        source_status(g, "#[r#\xE2\x80\xA8# note\xE2\x80\xA8#]r#\xE2\x80\xA8name")
            == SCAN_STATUS_COMPLETE);

  stream = scan_tokenize_owned(g, S("#[old#\n# note\n#]old#\nname"));
  check("comment-region streams support incremental edits",
        scan_stream_enable_edits(stream));
  while (scan_stream_next(stream)) {
  }
  check("incremental edits recompute region pairing",
        scan_stream_replace(stream, 2, 19, S("new#\n# note\n#]new"))
            && scan_stream_get(stream, 100) == nullptr
            && scan_status(stream->scanner) == SCAN_STATUS_COMPLETE);
  name = scan_stream_get(stream, 0);
  open = parse_token_leading_trivia(name);
  close = open->next->next;
  check("incremental recomputation retains membership metadata",
        open->region_id != 0 && open->region_id == close->region_id
            && parse_trivia_in_region(open->next, open->region_id));
  scan_token_stream_free(stream);
  parse_grammar_free(g);
}

static void test_string_families_expand_once(void) {
  static const char G[] =
      "@lex-string-family(plain, prefixes(\"r\", \"R\"), "
      "quotes(\"'\", '\"'), raw, as(\"STRING\"))\n"
      "<file> ::= %STRING\n";
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_borrow(G, sizeof(G) - 1);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  check("a declarative string family loads",
        load_test_grammar(text, CORE_STRING_STATIC("file"), g));
  kinds(g, "a string family expands prefixes across quote styles",
        "'a' \"b\" r'c' r\"d\" R'e' R\"f\"",
        "STRING_LIT STRING_LIT STRING_LIT STRING_LIT STRING_LIT "
        "STRING_LIT");

  parse_token_stream_t *ts = scan_tokenize_incremental_owned(g);
  check("a family prefix waits across an incremental boundary",
        scan_stream_append(ts, S("R")) && scan_stream_next(ts) == nullptr);
  check("a family string resumes without rescanning its prefix",
        scan_stream_append(ts, S("\"body\" ")) &&
            scan_stream_next(ts) != nullptr);

  scan_token_stream_free(ts);
  parse_grammar_free(g);
}

#define family_error(label, source, expected) \
  family_error_check((label), S(source), S(expected))

static void family_error_check(const char *label, core_string_t text,
                               core_string_t expected) {
  parse_grammar_t *g = parse_grammar_new();

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = load_test_grammar(text, CORE_STRING_STATIC("file"), g);
  scan_lex_t *lex = loaded ? scan_grammar_lex_compiled(g) : nullptr;
  core_string_t error = scan_lex_build_error();

  check(label, loaded && !lex && core_string_find(error, expected) >= 0);

  parse_grammar_free(g);
}

static void test_string_families_diagnose_invalid_declarations(void) {
  family_error("string families reject duplicate openers",
               "@lex-string('r\"', '\"', raw, as(\"STRING\"))\n"
               "@lex-string-family(prefixes(\"r\"), quotes('\"'), raw, "
               "as(\"STRING\"))\n<file> ::= %STRING\n",
               "duplicate string opener");
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
  static const char G[] =
      "@lex-string-family(prefixes(\"%q\", \"%w\"), "
      "pair(\"(\", \")\"), pair(\"[\", \"]\"), "
      "pair(\"{\", \"}\"), pair(\"<\", \">\"), nested, "
      "multiline, escape(\"\\\\\"), as(\"Q\"))\n"
      "<file> ::= %Q\n";
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_borrow(G, sizeof(G) - 1);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  check("paired string grammar loads",
        load_test_grammar(text, CORE_STRING_STATIC("file"), g));
  kinds(g, "paired strings balance their own delimiter",
        "%q(a(b)c) %q[a[b]c] %q{a{b}c} %q<a<b>c> "
        "%w(one(two)three)",
        "Q Q Q Q Q");
  kinds(g, "escaped paired delimiters do not affect nesting", "%q(a\\)b)", "Q");

  parse_token_stream_t *ts = scan_tokenize_incremental_owned(g);
  check(
      "nested paired strings retain depth across chunks",
      scan_stream_append(ts, S("%q(a(b")) && scan_stream_next(ts) == nullptr &&
          scan_stream_append(ts, S(")c) ")) && scan_stream_next(ts) != nullptr);
  scan_token_stream_free(ts);

  ts = scan_tokenize_incremental_owned(g);
  bool deep =
      scan_stream_append(ts, S("%q(")) && scan_stream_next(ts) == nullptr;
  for (size_t i = 0; deep && i < 512; i++) {
    deep = scan_stream_append(ts, S("(")) && scan_stream_next(ts) == nullptr;
  }
  for (size_t i = 0; deep && i < 512; i++) {
    deep = scan_stream_append(ts, S(")")) && scan_stream_next(ts) == nullptr;
  }
  deep = deep && scan_stream_append(ts, S(") ")) &&
         scan_stream_next(ts) != nullptr;
  check("deep paired nesting is iterative and incrementally resumable", deep);
  scan_token_stream_free(ts);

  check("an unbalanced paired string remains incomplete",
        source_status(g, "%q(a(b)c") == SCAN_STATUS_INCOMPLETE);

  parse_grammar_free(g);
}

static void test_composable_string_escape_profiles(void) {
  static const char G[] =
      "@lex-string-family(plain, quotes('\"', \"'\"), "
      "escape(\"\\\\\"), use_escapes(\"simple\", \"unicode\"), "
      "as(\"STRING\"))\n"
      "@lex-string-escapes(\"simple\", escapes(\"nrt\"), "
      "escape_digits(\"x\", \"0123456789abcdefABCDEF\", \"2\"))\n"
      "@lex-string-escapes(\"unicode\", "
      "escape_digits(\"u\", \"0123456789abcdefABCDEF\", \"4\"))\n"
      "<file> ::= %STRING\n";
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_borrow(G, sizeof(G) - 1);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = load_test_grammar(text, CORE_STRING_STATIC("file"), g);

  check("string escape profiles resolve before their users", loaded);
  check("composed escape profiles accept simple and fixed-width escapes",
        source_status(g, "\"\\n\\x4f\\u0041\"") == SCAN_STATUS_COMPLETE);
  check("composed escape profiles reject unknown escapes",
        source_status(g, "\"\\q\"") == SCAN_STATUS_INVALID);
  check("fixed-width escape profiles reject short bodies",
        source_status(g, "\"\\x4\"") == SCAN_STATUS_INVALID);

  parse_token_stream_t *ts = scan_tokenize_incremental_owned(g);
  check("a profiled escape waits across an incremental boundary",
        scan_stream_append(ts, S("\"\\u00")) &&
            scan_stream_next(ts) == nullptr &&
            scan_stream_append(ts, S("41\" ")) &&
            scan_stream_next(ts) != nullptr);
  scan_token_stream_free(ts);

  parse_grammar_free(g);

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

static void test_literal_string_escape_prefixes(void) {
  static const char G[] =
      "@lex-string-escapes(\"rich\", escapes(\"n\"), "
      "escape_literal(\"«\"), escape_literal(\"»\"), "
      "escape_literal(\"[\"))\n"
      "@lex-string('\"', '\"', escape(\"\\\\\"), "
      "use_escapes(\"rich\"), as(\"STRING\"))\n"
      "<file> ::= %STRING\n";
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_borrow(G, sizeof(G) - 1);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = load_test_grammar(text, CORE_STRING_STATIC("file"), g);
  check("exact multi-byte string escape prefixes compile",
        loaded && scan_grammar_lex_compiled(g));
  check("exact multi-byte string escape prefixes are accepted",
        loaded && source_status(g, "\"\\«\\»\\[\"")
            == SCAN_STATUS_COMPLETE);
  check("exact escape prefixes do not admit unrelated escapes",
        loaded && source_status(g, "\"\\q\"") == SCAN_STATUS_INVALID);
  parse_grammar_free(g);

  g = parse_grammar_new();
  text = S("@lex-string-escapes(\"bad\", escape_literal(\"\"))\n"
           "<file> ::= %STRING\n");
  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool bad_loaded = load_test_grammar(text, CORE_STRING_STATIC("file"), g);
  bool rejected = !bad_loaded || !scan_grammar_lex_compiled(g);
  check("exact string escape prefixes cannot be empty", rejected);
  parse_grammar_free(g);
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
  bool loaded = load_test_grammar(text, CORE_STRING_STATIC("file"), g);
  scan_lex_t *lex = loaded ? scan_grammar_lex_compiled(g) : nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts =
      lex ? scan_tokenize(g, S("p079\"body\""), nullptr, &sc) : nullptr;

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
  bool loaded = load_test_grammar(text, CORE_STRING_STATIC("file"), g);
  scan_lex_t *lex = loaded ? scan_grammar_lex_compiled(g) : nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts =
      lex ? scan_tokenize(g, S("1z079"), nullptr, &sc) : nullptr;
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
  bool loaded = load_test_grammar(text, CORE_STRING_STATIC("file"), g);

  parse_token_stream_t *ts =
      loaded ? scan_tokenize_incremental_owned(g) : nullptr;
  bool suffix_ok =
      ts && scan_stream_append(ts, S("1")) && scan_stream_next(ts) == nullptr;
  for (size_t i = 0; suffix_ok && i < 512; i++) {
    suffix_ok =
        scan_stream_append(ts, S("z")) && scan_stream_next(ts) == nullptr;
  }
  suffix_ok = suffix_ok && scan_stream_append(ts, S(" ")) &&
              scan_stream_next(ts) != nullptr &&
              scan_work_bytes(ts->scanner) < 2048;
  check("a long numeric suffix resumes in linear work", suffix_ok);
  scan_token_stream_free(ts);

  ts = loaded ? scan_tokenize_incremental_owned(g) : nullptr;
  bool prefix_ok =
      ts && scan_stream_append(ts, S("0")) && scan_stream_next(ts) == nullptr;
  for (size_t i = 1; prefix_ok && i < 512; i++) {
    prefix_ok =
        scan_stream_append(ts, S("x")) && scan_stream_next(ts) == nullptr;
  }
  prefix_ok = prefix_ok && scan_stream_append(ts, S("1 ")) &&
              scan_stream_next(ts) != nullptr &&
              scan_work_bytes(ts->scanner) < 2048;
  check("a long numeric prefix resumes in linear work", prefix_ok);
  scan_token_stream_free(ts);

  parse_grammar_free(g);
  core_buffer_free(source);
}

static void test_an_unknown_option_is_named(void) {
  static const char G[] = "@lex-string(\"'\", \"'\", bogus)\n"
                               "<file> ::= %IDENTIFIER\n";

  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_borrow(G, sizeof(G) - 1);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  load_test_grammar(text, CORE_STRING_STATIC("file"), g);

  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;

  parse_token_stream_t *ts = scan_tokenize(g, S("a"), &buf, &sc);

  core_string_t err = scan_lex_build_error();

  check("an unknown @lex- option is reported", err.data != nullptr);
  check("and the message names it",
        core_string_find(err, S("bogus")) >= 0 &&
            core_string_find(err, S("lex-string")) >= 0);

  if (ts) {
    scan_token_stream_free(ts);
    scan_scanner_free(sc);
    core_buffer_free(buf);
  }

  parse_grammar_free(g);

  g = parse_grammar_new();
  text = S("@lex-regex(\"/\", \"/\", not_after(\"TYPO\"), as(\"REGEX\"))\n"
           "<file> ::= %REGEX\n");
  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  load_test_grammar(text, CORE_STRING_STATIC("file"), g);

  ts = scan_tokenize(g, S("/x/"), &buf, &sc);
  err = scan_lex_build_error();

  check("an unknown regex predecessor token is rejected",
        core_string_find(err, S("TYPO")) >= 0 &&
            core_string_find(err, S("not_after")) >= 0);

  if (ts) {
    scan_token_stream_free(ts);
    scan_scanner_free(sc);
    core_buffer_free(buf);
  }

  parse_grammar_free(g);
}

static void test_lex_declarations_have_no_fixed_limit(void) {
  core_buffer_t *source = core_buffer_empty();

  for (size_t i = 0; i < 80; i++) {
    core_buffer_printf(source, "@lex-comment(\"#%zu\")\n", i);
    core_buffer_printf(source, "@lex-string(\"s%zu\")\n", i);
    core_buffer_printf(source,
                       "@lex-number(prefix(\"r%zu\"), digits(\"01\"), "
                       "int(\"INTEGER\"), suffix(\"z%zu\"))\n",
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

  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = {.data = source->data, .u8_bytes = source->byte_len};
  core_list_t(scan_lex_decl_t) decls = core_list_new(scan_lex_decl_t);

  parse_grammar_set_error_recovery(g, false);
  bool loaded = scan_lex_parse_file(text, S("many.lex"), &decls);
  check("many lexer declarations load", loaded);

  const scan_lex_spec_t *spec =
      loaded ? scan_lex_spec_from_decls(g, &decls) : nullptr;
  scan_lex_decls_free(&decls);

  check("many lexer declarations build", spec != nullptr);
  if (spec) {
    check("all comments are retained",
          spec->comments[79].opener.data != nullptr);
    check("all strings are retained", spec->strings[79].opener.data != nullptr);
    check("all number forms are retained",
          spec->number_forms[79].prefix.data != nullptr);
    check("all number suffixes are retained",
          spec->number_forms[79].suffixes[0].suffix.data != nullptr);
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
          spec->number_forms[79].suffixes[1].suffix.data == nullptr);
  }

  scan_lex_spec_free(spec);
  parse_grammar_free(g);
  core_buffer_free(source);
}

static parse_grammar_t *fence_grammar(void) {
  static const char G[] =
      "@lex-fence(\"[\", \"]\", fill(\"=\"), body(\"[\", \"]\"), "
      "minimum(\"1\"), label, as(\"EMBED\"))\n"
      "<file> ::= %EMBED\n";

  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_borrow(G, sizeof(G) - 1);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);

  if (!load_test_grammar(text, CORE_STRING_STATIC("file"), g)) {
    parse_grammar_free(g);
    g = nullptr;
  }

  return g;
}

static parse_grammar_t *hash_fence_grammar(void) {
  static const char G[] =
      "@lex-fence(\"#\", \"#\", fill(\"#\"), body('\"', '\"'), "
      "minimum(\"0\"), as(\"RAW\"))\n"
      "@lex-sigil(\"r#\", as(\"IDENTIFIER\"))\n"
      "@lex-ident(as(\"IDENTIFIER\"))\n"
      "<file> ::= %RAW | %IDENTIFIER\n";

  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_borrow(G, sizeof(G) - 1);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);

  if (!load_test_grammar(text, CORE_STRING_STATIC("file"), g)) {
    parse_grammar_free(g);
    g = nullptr;
  }

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
      scan_tokenize(g, core_buffer_string(source), nullptr, &sc);
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
  core_string_t source = S("[==codec[one ]=] two]==]");
  parse_grammar_t *g = fence_grammar();
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize(g, source, &buf, &sc);
  parse_token_info_t *tok = scan_stream_next(ts);

  check("a parameterized symmetric fence is one token",
        tok && scan_stream_next(ts) == nullptr);
  check("a fence retains its exact body span",
        tok && has_content(tok) &&
            core_string_eq(core_string_slice(source, content_start(tok),
                                             content_end(tok) -
                                                 content_start(tok)),
                           S("one ]=] two")));
  check("a fence retains its optional label span",
        tok && has_label(tok) &&
            core_string_eq(core_string_slice(source, label_start(tok),
                                             label_end(tok) -
                                                 label_start(tok)),
                           S("codec")));
  check("a closed fence is complete", scan_status(sc) == SCAN_STATUS_COMPLETE);

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
}

static void test_fence_padding_requires_both_boundaries(void) {
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text =
      S("@lex-fence(\"[\", \"]\", fill(\"-\"), pad(\"required\"), "
        "as(\"FENCE\"))\n"
        "@lex-fallback(\"reject\")\n"
        "<file> ::= %FENCE\n");
  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = load_test_grammar(text, S("file"), g);
  check("a padded fence declaration loads", loaded);
  if (!loaded) {
    parse_grammar_free(g);
    return;
  }

  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize(g, S("[- -]"), &buf, &sc);
  parse_token_info_t *tok = scan_stream_next(ts);
  check("required padding accepts a space-padded empty literal",
        tok && has_content(tok) &&
            content_start(tok) == content_end(tok) &&
            scan_stream_next(ts) == nullptr);
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  buf = nullptr;
  sc = nullptr;
  ts = scan_tokenize(g, S("[- f-]"), &buf, &sc);
  bool emitted_fence = false;
  while ((tok = scan_stream_next(ts))) {
    emitted_fence |= parse_token_tid(tok) == parse_terminal_id(g, S("FENCE"));
  }
  check("required padding rejects a closing fence without boundary space",
        !emitted_fence && scan_status(sc) == SCAN_STATUS_INCOMPLETE);
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
}

static void test_incomplete_fences_resume_after_append(void) {
  core_string_t src = S("[=ffi[body");
  parse_grammar_t *g = fence_grammar();
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize(g, src, &buf, &sc);

  check("an incomplete fence emits no provisional token",
        scan_stream_next(ts) == nullptr && scan_stream_token_count(ts) == 0);
  check("an incomplete fence retains its source span",
        scan_status(sc) == SCAN_STATUS_INCOMPLETE &&
            sc->status_start_byte == 0 &&
            sc->status_end_byte == src.u8_bytes);
  check("an incomplete stream accepts more source",
        scan_stream_append(ts, S("]=]")));

  parse_token_info_t *tok = scan_stream_next(ts);

  check("the resumed fence is emitted once",
        tok && scan_stream_next(ts) == nullptr &&
            scan_stream_token_count(ts) == 1);
  check("the resumed fence retains its complete body span",
        tok && has_content(tok) &&
            content_end(tok) - content_start(tok) == 4 &&
            !memcmp(buf->data + content_start(tok), "body", 4));
  check("the resumed stream completes",
        scan_status(sc) == SCAN_STATUS_COMPLETE);
  check("a complete stream rejects further append",
        !scan_stream_append(ts, S("x")));

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
}

static void test_incomplete_strings_and_comments_resume(void) {
  static const core_string_t TERMS[] = {
      S("IDENTIFIER"),
      S("STRING"),
      {0},
  };
  parse_grammar_t *g = mk(&JS, TERMS);
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize(g, S("\"hel"), &buf, &sc);

  check("an incomplete string emits no provisional token",
        scan_stream_next(ts) == nullptr && scan_stream_token_count(ts) == 0);
  check("a string can remain incomplete across appends",
        scan_stream_append(ts, S("lo")) && scan_stream_next(ts) == nullptr &&
            scan_status(sc) == SCAN_STATUS_INCOMPLETE);
  check("a later append can complete the string",
        scan_stream_append(ts, S("\"")));

  parse_token_info_t *string = scan_stream_next(ts);

  check("the resumed string contains both source fragments",
        string && parse_token_has_value(string) &&
            core_option_get(parse_token_value(string)).u8_bytes == 7 &&
            !memcmp(core_option_get(parse_token_value(string)).data, "\"hello\"", 7));

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  buf = nullptr;
  sc = nullptr;
  ts = scan_tokenize(g, S("/* open"), &buf, &sc);

  check("an incomplete block comment emits no token",
        scan_stream_next(ts) == nullptr &&
            scan_status(sc) == SCAN_STATUS_INCOMPLETE);
  check("a block comment can resume after append",
        scan_stream_append(ts, S(" */ name")));

  parse_token_info_t *name = scan_stream_next(ts);

  check("scanning continues after a resumed block comment",
        name && parse_token_has_value(name) &&
            core_string_eq(core_option_get(parse_token_value(name)),
                           CORE_STRING_STATIC("name")) &&
            parse_token_leading_trivia(name) &&
            core_string_eq(parse_token_leading_trivia(name)->text,
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
  parse_grammar_t *g = parse_grammar_new();
  core_string_t grammar_text = S("@lex-comment(\"/\", \"/\", fill(\"*\"))\n"
                                 "@lex-ident(as(\"IDENTIFIER\"))\n"
                                 "<file> ::= %IDENTIFIER\n");
  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, grammar_text);
  bool loaded = load_test_grammar(grammar_text, S("file"), g);
  check("a repeated-fill block comment declaration loads", loaded);
  if (!loaded) {
    parse_grammar_free(g);
    return;
  }

  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts =
      scan_tokenize(g, S("/** /* Hi! */ **/ name"), &buf, &sc);
  parse_token_info_t *name = scan_stream_next(ts);
  check("the closing star count matches the opening star count",
        name && parse_token_has_value(name) &&
            core_string_eq(core_option_get(parse_token_value(name)), S("name")) &&
            parse_token_leading_trivia(name) &&
            core_string_eq(parse_token_leading_trivia(name)->text, S("/** /* Hi! */ **/")));
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  buf = nullptr;
  sc = nullptr;
  ts = scan_tokenize(g, S("/** body */"), &buf, &sc);
  check("a closing star count that differs is not accepted",
        scan_stream_next(ts) == nullptr &&
            scan_status(sc) == SCAN_STATUS_INCOMPLETE);
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  buf = nullptr;
  sc = nullptr;
  ts = scan_tokenize(g, S("/** body **"), &buf, &sc);
  check("a repeated-fill close may be split across input chunks",
        scan_stream_next(ts) == nullptr &&
            scan_status(sc) == SCAN_STATUS_INCOMPLETE &&
            scan_stream_append(ts, S("/ name")));
  name = scan_stream_next(ts);
  check("the resumed filled comment closes at the matching count",
        name && parse_token_has_value(name) &&
            core_string_eq(core_option_get(parse_token_value(name)), S("name")));
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
}

static void test_interpolation_regex_and_heredoc_resume(void) {
  static const core_string_t JS_TERMS[] = {
      S("IDENTIFIER"),    S("TEMPLATE_HEAD"), S("TEMPLATE_MIDDLE"),
      S("TEMPLATE_TAIL"), S("REGEX_LIT"),     {0},
  };
  parse_grammar_t *g = mk(&JS, JS_TERMS);
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize(g, S("`a${x}"), &buf, &sc);
  parse_token_info_t *head = scan_stream_next(ts);
  parse_token_info_t *hole = scan_stream_next(ts);

  check("an interpolated tail can stop incomplete",
        head && hole && scan_stream_next(ts) == nullptr &&
            scan_status(sc) == SCAN_STATUS_INCOMPLETE);
  check("an interpolated tail can resume after append",
        scan_stream_append(ts, S("b`")));

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
  ts = scan_tokenize(g, S("/ab"), &buf, &sc);

  check("an unterminated regex emits no provisional token",
        scan_stream_next(ts) == nullptr &&
            scan_status(sc) == SCAN_STATUS_INCOMPLETE);
  check("a regex can resume after append", scan_stream_append(ts, S("+/gi")));

  parse_token_info_t *regex = scan_stream_next(ts);

  check("the resumed regex is emitted once",
        regex && scan_stream_next(ts) == nullptr &&
            scan_stream_token_count(ts) == 1 &&
            scan_status(sc) == SCAN_STATUS_COMPLETE);

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);

  static const core_string_t RB_TERMS[] = {
      S("IDENTIFIER"),
      S("HEREDOC"),
      {0},
  };

  g = mk(&RUBY, RB_TERMS);
  buf = nullptr;
  sc = nullptr;
  ts = scan_tokenize(g, S("<<~END\nbody\n"), &buf, &sc);

  check("an unterminated heredoc emits no provisional token",
        scan_stream_next(ts) == nullptr &&
            scan_status(sc) == SCAN_STATUS_INCOMPLETE &&
            sc->status_end_byte == 12);
  check("a heredoc can resume after append",
        scan_stream_append(ts, S("END\nnext")));

  parse_token_info_t *heredoc = scan_stream_next(ts);
  parse_token_info_t *next = scan_stream_next(ts);

  check("the resumed heredoc preserves following source",
        heredoc && next && scan_stream_next(ts) == nullptr &&
            parse_token_has_value(next) &&
            core_string_eq(core_option_get(parse_token_value(next)),
                           CORE_STRING_STATIC("next")));

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
}

static void test_incremental_stream_accepts_arbitrary_chunks(void) {
  static const core_string_t TERMS[] = {
      S("IDENTIFIER"), S("INTEGER"), S("FLOAT"), S("STRING"), {0},
  };
  parse_grammar_t *g = mk(&JS, TERMS);
  parse_register_terminal(g, S("==="));
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize_incremental(g, &buf, &sc);

  bool first_append = scan_stream_append(ts, S("iden"));
  parse_token_info_t *first_token = scan_stream_next(ts);
  check("an identifier waits at a temporary buffer boundary",
        first_append && first_token == nullptr &&
            scan_status(sc) == SCAN_STATUS_INCOMPLETE);
  check("an identifier continues in the next chunk",
        scan_stream_append(ts, S("t ")) && scan_stream_next(ts) != nullptr);

  check("a terminal prefix waits for a longer terminal",
        scan_stream_append(ts, S("=")) && scan_stream_next(ts) == nullptr);
  check("the longest terminal completes across chunks",
        scan_stream_append(ts, S("==")) && scan_stream_next(ts) != nullptr);

  check("a radix number waits across chunks",
        scan_stream_append(ts, S(" 0")) && scan_stream_next(ts) == nullptr &&
            scan_stream_append(ts, S("x1")) && scan_stream_next(ts) == nullptr);
  parse_token_info_t *hex = nullptr;
  check("a radix number ends when a delimiter arrives",
        scan_stream_append(ts, S("f ")) &&
            (hex = scan_stream_next(ts)) != nullptr &&
            core_option_get(parse_token_value(hex)).u8_bytes == 4);

  check("an exponent resumes after its sign",
        scan_stream_append(ts, S("1.2e")) && scan_stream_next(ts) == nullptr &&
            scan_stream_append(ts, S("+")) && scan_stream_next(ts) == nullptr &&
            scan_stream_append(ts, S("3 ")) && scan_stream_next(ts) != nullptr);

  check("a line comment continues across chunks",
        scan_stream_append(ts, S("// com")) &&
            scan_stream_next(ts) == nullptr &&
            scan_stream_append(ts, S("ment\nname ")) &&
            scan_stream_next(ts) != nullptr);

  check("finishing converts a temporary boundary to final EOF",
        scan_stream_finish(ts) && scan_stream_next(ts) == nullptr &&
            scan_status(sc) == SCAN_STATUS_COMPLETE);
  check("a finished stream rejects more input",
        !scan_stream_append(ts, S("x")));

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
}

static void test_incremental_utf8_and_source_compaction(void) {
  static const core_string_t TERMS[] = {
      S("IDENTIFIER"), S("INTEGER"), S("FLOAT"), S("STRING"), {0},
  };
  parse_grammar_t *g = mk(&JS, TERMS);
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize_incremental(g, &buf, &sc);
  core_string_t lambda = S("\xCE\xBB");

  check("a partial UTF-8 scalar waits for its remaining bytes",
        scan_stream_append(ts, core_string_slice(lambda, 0, 1)) &&
            scan_stream_next(ts) == nullptr && sc->cursor == 0);
  check("a Unicode identifier resumes across a byte boundary",
        scan_stream_append(ts, core_string_slice(lambda, 1, 1)) &&
            scan_stream_next(ts) == nullptr && scan_stream_append(ts, S(" ")));

  parse_token_info_t *first = scan_stream_next(ts);
  check("the resumed Unicode token preserves its complete spelling",
        first && parse_token_has_value(first) &&
            core_string_eq(core_option_get(parse_token_value(first)), lambda));
  check("consumed incremental source can be discarded",
        scan_stream_discard_consumed(ts) && sc->source_base == 2 &&
            buf->byte_len == 1);

  parse_token_info_t *second = nullptr;
  check("absolute source spans survive compaction",
        scan_stream_append(ts, S("name ")) &&
            (second = scan_stream_next(ts)) != nullptr);
  check("a post-compaction token uses the original byte coordinate space",
        second && parse_token_start_byte(second) == 3 &&
            parse_token_end_byte(second) == 7);
  check("discarding consumed input also bounds the rewind cache",
        scan_stream_discard_consumed(ts) && ts->token_count <= 1);

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
}

static void test_source_compaction_is_amortized(void) {
  static const core_string_t TERMS[] = {
      S("IDENTIFIER"), S("INTEGER"), S("FLOAT"), S("STRING"), {0},
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
  scan_stream_append(ts, core_buffer_string(source));
  check("the first token in a large incremental window is available",
        scan_stream_next(ts) != nullptr);
  check("discard advances the logical window without shifting its suffix",
        scan_stream_discard_consumed(ts) && sc->input_offset == 1 &&
            sc->input.u8_bytes == original - 1 && buf->byte_len == original);

  size_t tokens = 1;
  while (scan_stream_next(ts)) {
    tokens++;
    scan_stream_discard_consumed(ts);
  }

  check("geometric compaction preserves every token", tokens == 4096);
  check("geometric compaction bounds the retained source prefix",
        sc->input_offset < sc->input.u8_bytes || sc->input.u8_bytes <= 1);

  core_buffer_free(source);
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
}

static bool token_value_matches(parse_token_info_t *token,
                                core_string_t text) {
  if (!token || !parse_token_has_value(token)) {
    return false;
  }
  return core_string_eq(core_option_get(parse_token_value(token)), text);
}

#define token_value_is(token, text) token_value_matches((token), S(text))

static size_t buffer_offset(parse_token_stream_t *ts, core_string_t needle) {
  return (size_t)core_string_find(core_buffer_string(ts->scanner->buffer),
                                  needle);
}

static void test_incremental_source_edits(void) {
  static const core_string_t TERMS[] = {
      S("IDENTIFIER"), S("INTEGER"), S("FLOAT"), S("STRING"), {0},
  };
  parse_grammar_t *g = mk(&JS, TERMS);
  core_buffer_t *source = core_buffer_empty();
  for (size_t i = 0; i < 80; i++) {
    core_buffer_printf(source, "a%zu ", i);
  }
  parse_token_stream_t *ts =
      scan_tokenize_owned(g, core_buffer_string(source));
  check("source editing can be enabled before tokenization",
        scan_stream_enable_edits(ts));
  while (scan_stream_next(ts)) {
  }
  parse_token_info_t *stable_prefix = scan_stream_get(ts, 5);
  parse_token_info_t *stable_suffix = scan_stream_get(ts, 70);
  size_t start = buffer_offset(ts, S("a40"));

  check("a replacement restarts from a lexical checkpoint",
        scan_stream_replace(ts, start, start + 3, S("renamed")));
  parse_token_info_t *changed = scan_stream_get(ts, 40);
  check("replacement preserves stable prefix token identities",
        scan_stream_get(ts, 5) == stable_prefix &&
            token_value_is(changed, "renamed"));
  scan_stream_get(ts, 1000);
  check("replacement reconciles unchanged suffix token identities",
        scan_stream_get(ts, 70) == stable_suffix);

  start = buffer_offset(ts, S("a41 "));
  check("deletion retokenizes the affected suffix",
        scan_stream_delete(ts, start, start + 4) &&
            token_value_is(scan_stream_get(ts, 41), "a42"));

  start = buffer_offset(ts, S("a42"));
  check("insertion participates in normal longest-match scanning",
        scan_stream_insert(ts, start, S("inserted ")) &&
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
  ts = scan_tokenize_owned(g, S("\"open"));
  scan_stream_enable_edits(ts);
  while (scan_stream_next(ts)) {
  }
  check("an edit can complete a formerly incomplete construct",
        scan_status(ts->scanner) == SCAN_STATUS_INCOMPLETE &&
            scan_stream_replace(ts, 5, 5, S("\"")));
  scan_stream_get(ts, 1000);
  check("completion state is recomputed after insertion",
        scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);
  check("deleting a closer can make a construct incomplete again",
        scan_stream_replace(ts, 5, 6, (core_string_t){0}));
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
  ts = scan_tokenize_owned(g, core_buffer_string(source));
  scan_stream_enable_edits(ts);
  while (scan_stream_next(ts)) {
  }
  size_t work_before = scan_work_bytes(ts->scanner);
  bool bounded = true;
  size_t edit_at = 2048 * 5;
  for (size_t i = 0; i < 100; i++) {
    core_string_t byte = i & 1 ? S("a") : S("b");
    bounded = bounded &&
              scan_stream_replace(ts, edit_at, edit_at + 1, byte) &&
              scan_stream_get(ts, 4095) != nullptr;
  }
  size_t edit_work = scan_work_bytes(ts->scanner) - work_before;
  check("repeated fixed-size edits converge without quadratic rescanning",
        bounded && edit_work < 100 * 1024);
  scan_token_stream_free(ts);
  core_buffer_free(source);
  parse_grammar_free(g);

  static const core_string_t JS_TERMS[] = {
      S("IDENTIFIER"),    S("INTEGER"),         S("FLOAT"),
      S("STRING"),        S("TEMPLATE_HEAD"),   S("TEMPLATE_MIDDLE"),
      S("TEMPLATE_TAIL"), {0},
  };
  g = mk(&JS, JS_TERMS);
  source = core_buffer_empty();
  for (size_t i = 0; i < 30; i++) {
    core_buffer_puts(source, "p ");
  }
  core_buffer_puts(source, "`head${x}tail`");
  ts = scan_tokenize_owned(g, core_buffer_string(source));
  scan_stream_enable_edits(ts);
  while (scan_stream_next(ts)) {
  }
  start = buffer_offset(ts, S("tail"));
  check("an edit resumes from checkpointed interpolation state",
        scan_stream_replace(ts, start, start + 4, S("ending")));
  while (scan_stream_next(ts)) {
  }
  check("interpolation state recovers after an edit",
        scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);
  scan_token_stream_free(ts);
  core_buffer_free(source);
  parse_grammar_free(g);

  static const core_string_t PY_EDIT_TERMS[] = {
      S("NEWLINE"),        S("INDENT"),      S("DEDENT"), S("ENDMARKER"),
      S("IDENTIFIER"),     S("INTEGER"),     S("FLOAT"),  S("STRING"),
      S("IMAGINARY"),      S("FSTRING_START"), S("FSTRING_MIDDLE"),
      S("FSTRING_END"),    {0},
  };
  g = mk(&PYTHON, PY_EDIT_TERMS);
  ts = scan_tokenize_owned(g, S("root\n  a\n  b\n  c\nend\n"));
  scan_stream_enable_edits(ts);
  while (scan_stream_next(ts)) {
  }
  start = buffer_offset(ts, S("  b"));
  check("indentation state is rebuilt after an edit",
        scan_stream_replace(ts, start, start + 2, S("    ")));
  while (scan_stream_next(ts)) {
  }
  check("edited indentation produces a deterministic result",
        scan_status(ts->scanner) != SCAN_STATUS_INCOMPLETE);
  scan_token_stream_free(ts);
  parse_grammar_free(g);

  g = embedded_grammar(S("json"), true);
  source = core_buffer_empty();
  for (size_t i = 0; i < 40; i++) {
    core_buffer_puts(source, "outer ");
  }
  core_buffer_puts(source, "{{1}} tail");
  ts = scan_tokenize_owned(g, core_buffer_string(source));
  scan_stream_enable_edits(ts);
  while (scan_stream_next(ts)) {
  }
  start = buffer_offset(ts, S("{{1}}")) + 2;
  check("embedded lexer state recovers after an inner edit",
        scan_stream_replace(ts, start, start + 1, S("22")));
  scan_stream_get(ts, 1000);
  check("an edited embedded stream converges and completes",
        scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);
  start = buffer_offset(ts, S("22"));
  check("a second inner edit can invalidate an embedded language",
        scan_stream_replace(ts, start, start + 2, S("\"open")));
  scan_stream_get(ts, 1000);
  scan_diagnostic_t edited_embed_diagnostic = {0};
  check("post-edit inner diagnostics retain embedded lexer identity",
        scan_diagnostic(ts->scanner, &edited_embed_diagnostic) &&
            core_string_eq(edited_embed_diagnostic.lexer, S("json")));
  scan_token_stream_free(ts);
  core_buffer_free(source);
  parse_grammar_free(g);

  g = fence_grammar();
  ts = scan_tokenize_owned(g, S("[==ffi[body]==]"));
  scan_stream_enable_edits(ts);
  while (scan_stream_next(ts)) {
  }
  start = buffer_offset(ts, S("body"));
  check("an edit inside a variable-width fence invalidates that token",
        scan_stream_replace(ts, start, start + 4, S("changed")));
  parse_token_info_t *fence = scan_stream_get(ts, 0);
  scan_stream_get(ts, 1000);
  check("fence state and content spans recover after an edit",
        fence && has_content(fence) &&
            scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);
  scan_token_stream_free(ts);
  parse_grammar_free(g);

  static const core_string_t RB_EDIT_TERMS[] = {
      S("IDENTIFIER"), S("INTEGER"), S("HEREDOC"), S("CVAR"),
      S("IVAR"),       S("GVAR"),    {0},
  };
  g = mk(&RUBY, RB_EDIT_TERMS);
  ts = scan_tokenize_owned(g, S("x = <<~END\n  body\nEND\ny\n"));
  scan_stream_enable_edits(ts);
  while (scan_stream_next(ts)) {
  }
  start = buffer_offset(ts, S("body"));
  check("an edit inside a heredoc invalidates and rebuilds its body",
        scan_stream_replace(ts, start, start + 4, S("revised")));
  scan_stream_get(ts, 1000);
  check("heredoc queues recover after an edit",
        scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);
  scan_token_stream_free(ts);
  parse_grammar_free(g);
}

static size_t scan_one_byte_chunks(parse_grammar_t *g, core_string_t source) {
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize_incremental(g, nullptr, &sc);

  for (size_t i = 0; i < source.u8_bytes; i++) {
    if (!scan_stream_append(ts, core_string_slice(source, i, 1))) {
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
                                          core_string_t source,
                                          int32_t wanted_tid) {
  parse_token_stream_t *ts = scan_tokenize_incremental(g, nullptr, nullptr);
  size_t count = 0;

  for (size_t i = 0; i < source.u8_bytes; i++) {
    scan_stream_append(ts, core_string_slice(source, i, 1));
    parse_token_info_t *tok = nullptr;
    while ((tok = scan_stream_next(ts))) {
      count += parse_token_tid(tok) == wanted_tid;
    }
  }
  scan_stream_finish(ts);
  parse_token_info_t *tok = nullptr;
  while ((tok = scan_stream_next(ts))) {
    count += parse_token_tid(tok) == wanted_tid;
  }
  scan_token_stream_free(ts);
  return count;
}

static void test_incremental_scanning_is_linear(void) {
  static const core_string_t TERMS[] = {
      S("IDENTIFIER"), S("INTEGER"), S("FLOAT"), S("STRING"), {0},
  };
  parse_grammar_t *js = mk(&JS, TERMS);
  parse_grammar_t *ruby = mk(&RUBY, TERMS);
  parse_grammar_t *fence = fence_grammar();
  parse_grammar_t *hash_fence = hash_fence_grammar();
  core_buffer_t *body = core_buffer_empty();

  for (size_t i = 0; i < 4096; i++) {
    core_buffer_putc(body, 'a');
  }

  core_buffer_t *string = core_buffer_empty();
  core_buffer_puts(string, "\"");
  core_buffer_append(string, body->data, body->byte_len);
  core_buffer_puts(string, "\" ");
  core_buffer_t *comment = core_buffer_empty();
  core_buffer_puts(comment, "//");
  core_buffer_append(comment, body->data, body->byte_len);
  core_buffer_puts(comment, "\nname ");
  core_buffer_t *heredoc = core_buffer_empty();
  core_buffer_puts(heredoc, "<<~END\n");
  core_buffer_append(heredoc, body->data, body->byte_len);
  core_buffer_puts(heredoc, "\nEND\nname ");
  core_buffer_t *fenced = core_buffer_empty();
  core_buffer_puts(fenced, "[=ffi[");
  core_buffer_append(fenced, body->data, body->byte_len);
  core_buffer_puts(fenced, "]=] ");
  core_buffer_t *hash_fenced = core_buffer_empty();
  core_buffer_puts(hash_fenced, "###\"");
  core_buffer_append(hash_fenced, body->data, body->byte_len);
  core_buffer_puts(hash_fenced, "\"### ");

  check("one-byte string chunks take linear scanner work",
        scan_one_byte_chunks(js, core_buffer_string(string)) <
            string->byte_len * 5);
  check("one-byte comment chunks take linear scanner work",
        scan_one_byte_chunks(js, core_buffer_string(comment)) <
            comment->byte_len * 5);
  check("one-byte heredoc chunks take linear scanner work",
        scan_one_byte_chunks(ruby, core_buffer_string(heredoc)) <
            heredoc->byte_len * 5);
  check("one-byte fenced chunks take linear scanner work",
        scan_one_byte_chunks(fence, core_buffer_string(fenced)) <
            fenced->byte_len * 5);
  check("one-byte variable hash fences take linear scanner work",
        scan_one_byte_chunks(hash_fence, core_buffer_string(hash_fenced)) <
            hash_fenced->byte_len * 5);
  check("a split comment opener is not mistaken for a regex",
        count_one_byte_chunk_tokens(js, S("//body\n"), PARSE_TOK_STRING_LIT) ==
            0);
  check("a split string opener still produces one string token",
        count_one_byte_chunk_tokens(
            js, S("\"body\""), (int32_t)parse_terminal_id(js, S("STRING"))) ==
            1);
  check("a split heredoc opener still produces one heredoc token",
        count_one_byte_chunk_tokens(
            ruby, S("<<~END\nbody\nEND\n"),
            (int32_t)parse_terminal_id(ruby, S("HEREDOC"))) == 1);
  check("a split fence opener still produces one fence token",
        count_one_byte_chunk_tokens(
            fence, S("[=ffi[body]=]"),
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
  static const core_string_t TERMS[] = {S("IDENTIFIER"), {0}};
  parse_grammar_t *g = mk(&JS, TERMS);
  parse_token_stream_t *ts = scan_tokenize_owned(g, S("name"));

  check("convenience tokenization returns a usable owned stream",
        ts && scan_stream_next(ts) != nullptr);
  scan_token_stream_free(ts);

  ts = scan_tokenize_incremental_owned(g);
  check("owned incremental tokenization needs no side-channel handles",
        ts && scan_stream_append(ts, S("name ")) &&
            scan_stream_next(ts) != nullptr);
  scan_token_stream_free(ts);
  parse_grammar_free(g);
}

static void test_terminals_have_no_byte_width_limit(void) {
  static const core_string_t TERMS[] = {S("IDENTIFIER"), {0}};
  parse_grammar_t *g = mk(&JS, TERMS);
  core_buffer_t *op = core_buffer_empty();

  for (size_t i = 0; i < 1024; i++) {
    core_buffer_putc(op, '<');
  }
  parse_register_terminal(g, core_buffer_string(op));

  parse_token_stream_t *ts =
      scan_tokenize(g, core_buffer_string(op), nullptr, nullptr);
  parse_token_info_t *tok = scan_stream_next(ts);
  check("the terminal trie matches a terminal longer than 255 bytes",
        tok && parse_token_has_value(tok) &&
            core_option_get(parse_token_value(tok)).u8_bytes == 1024);

  scan_token_stream_free(ts);
  parse_grammar_free(g);
  core_buffer_free(op);
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

static scan_status_t source_status_of(parse_grammar_t *g,
                                      core_string_t source) {
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize(g, source, &buf, &sc);

  while (scan_stream_next(ts)) {
  }

  scan_status_t result = scan_status(sc);

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  return result;
}

#define recovery_declaration_fails(source, expected) \
  recovery_declaration_check(S(source), S(expected))

static bool recovery_declaration_check(core_string_t text,
                                       core_string_t expected) {
  parse_grammar_t *grammar = parse_grammar_new();

  parse_grammar_set_error_recovery(grammar, false);
  parse_bnf_declare_terminals(grammar, text);
  bool loaded = load_test_grammar(text, S("file"), grammar);
  scan_lex_t *program = loaded ? scan_grammar_lex_compiled(grammar) : nullptr;
  core_string_t error = scan_lex_build_error();
  if (!error.data) {
    error = scan_lex_compile_error();
  }
  bool result = !program && core_string_find(error, expected) >= 0;

  parse_grammar_free(grammar);
  return result;
}

static void test_malformed_token_recovery(void) {
  static const core_string_t TERMS[] = {
      S("IDENTIFIER"), S("INTEGER"), S("STRING"), S("ERROR"), S(";"), {0},
  };
  core_string_t source = S("1__oops; next");
  parse_grammar_t *strict = mk(&RECOVERY_LEXER, TERMS);
  parse_token_stream_t *ts = scan_tokenize_owned(strict, source);

  while (scan_stream_next(ts)) {
  }
  check("batch scanning remains strict when recovery is declared",
        scan_status(ts->scanner) == SCAN_STATUS_INVALID);
  scan_token_stream_free(ts);
  parse_grammar_free(strict);

  parse_grammar_t *editor = mk(&RECOVERY_LEXER, TERMS);
  core_buffer_t *buffer = nullptr;
  parse_scanner_t *scanner = nullptr;
  ts = scan_tokenize(editor, source, &buffer, &scanner);
  scan_scanner_set_recovery(scanner, true);

  parse_token_info_t *error = scan_stream_next(ts);
  parse_token_info_t *semicolon = scan_stream_next(ts);
  parse_token_info_t *identifier = scan_stream_next(ts);
  parse_token_info_t *end = scan_stream_next(ts);
  int64_t error_tid = parse_lookup_terminal(editor, S("ERROR"));
  int64_t semicolon_tid = parse_lookup_terminal(editor, S(";"));

  check("editor recovery replaces the malformed token",
        error && parse_token_tid(error) == error_tid && parse_token_has_value(error) &&
            core_option_get(parse_token_value(error)).u8_bytes == 7 &&
            !memcmp(core_option_get(parse_token_value(error)).data, "1__oops", 7));
  check("recovery stops before an unconsumed synchronization token",
        semicolon && parse_token_tid(semicolon) == semicolon_tid && identifier &&
            parse_token_tid(identifier) == parse_lookup_terminal(editor, S("IDENTIFIER")));
  check("recovered input reaches EOF with a distinct status",
        !end && scan_status(scanner) == SCAN_STATUS_RECOVERED);

  scan_diagnostic_t diagnostic = {0};
  check("recovery retains the original malformed-token diagnostic",
        scan_diagnostic(scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_RECOVERED &&
            diagnostic.problem.kind == SCAN_DIAG_NUMBER_MALFORMED &&
            diagnostic.start_byte == 0);

  scan_token_stream_free(ts);
  scan_scanner_free(scanner);
  core_buffer_free(buffer);
  parse_grammar_free(editor);

  editor = mk(&RECOVERY_LEXER, TERMS);
  buffer = nullptr;
  scanner = nullptr;
  source = S("\"bad\\q\" trailing\nnext");
  ts = scan_tokenize(editor, source, &buffer, &scanner);
  scan_scanner_set_recovery(scanner, true);
  error = scan_stream_next(ts);
  identifier = scan_stream_next(ts);
  end = scan_stream_next(ts);

  check("string recovery synchronizes without consuming later lines",
        error && parse_token_tid(error) == parse_lookup_terminal(editor, S("ERROR")) &&
            parse_token_end_byte(error) == 16 && identifier &&
            parse_token_has_value(identifier) &&
            core_string_eq(core_option_get(parse_token_value(identifier)), S("next")) &&
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
  bool first_chunk = scan_stream_append(ts, S("1__oops;"));
  error = scan_stream_next(ts);
  check("a split synchronization delimiter keeps recovery incomplete",
        first_chunk && !error && scan_stream_token_count(ts) == 0 &&
            scan_status(scanner) == SCAN_STATUS_INCOMPLETE);
  bool second_chunk = scan_stream_append(ts, S("; next"));
  bool finished = scan_stream_finish(ts);
  error = scan_stream_next(ts);
  while (scan_stream_next(ts)) {
  }
  check("recovery resumes across a split synchronization delimiter",
        second_chunk && finished && error &&
            parse_token_tid(error) == parse_lookup_terminal(incremental, S("ERROR")) &&
            parse_token_has_value(error) &&
            core_option_get(parse_token_value(error)).u8_bytes == 7 &&
            scan_status(scanner) == SCAN_STATUS_RECOVERED);
  scan_token_stream_free(ts);
  scan_scanner_free(scanner);
  core_buffer_free(buffer);
  parse_grammar_free(incremental);

  parse_grammar_t *consuming = mk(&CONSUMING_RECOVERY_LEXER, TERMS);
  buffer = nullptr;
  scanner = nullptr;
  source = S("1__oops;next");
  ts = scan_tokenize(consuming, source, &buffer, &scanner);
  scan_scanner_set_recovery(scanner, true);
  error = scan_stream_next(ts);
  identifier = scan_stream_next(ts);
  end = scan_stream_next(ts);
  check("a recovery recipe may consume its synchronization delimiter",
        error && parse_token_tid(error) == parse_lookup_terminal(consuming, S("ERROR")) &&
            parse_token_end_byte(error) == 8 && identifier &&
            parse_token_has_value(identifier) &&
            core_string_eq(core_option_get(parse_token_value(identifier)), S("next")) &&
            !end && scan_status(scanner) == SCAN_STATUS_RECOVERED);
  scan_token_stream_free(ts);
  scan_scanner_free(scanner);
  core_buffer_free(buffer);
  parse_grammar_free(consuming);

  core_buffer_t *large_recovery = core_buffer_empty();
  core_buffer_puts(large_recovery, "1__");
  for (size_t i = 0; i < 1 << 18; i++) {
    core_buffer_putc(large_recovery, 'a');
  }
  core_buffer_puts(large_recovery, "; next");
  editor = mk(&RECOVERY_LEXER, TERMS);
  buffer = nullptr;
  scanner = nullptr;
  ts = scan_tokenize(editor, core_buffer_string(large_recovery), &buffer,
                     &scanner);
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
  source = S("1__oops; next");
  ts = scan_tokenize_owned(editor, source);
  bool recovery_enabled = scan_scanner_set_recovery(ts->scanner, true);
  bool edits_enabled = scan_stream_enable_edits(ts);
  while (scan_stream_next(ts)) {
  }
  bool initially_recovered = scan_status(ts->scanner) == SCAN_STATUS_RECOVERED;
  bool repaired = scan_stream_replace(ts, 0, 7, S("1"));
  scan_stream_get(ts, 1000);
  check("editing can repair a recovered token and clear recovery status",
        recovery_enabled && edits_enabled && initially_recovered && repaired &&
            scan_status(ts->scanner) == SCAN_STATUS_COMPLETE);
  scan_token_stream_free(ts);
  parse_grammar_free(editor);

  editor = mk(&REJECT_RECOVERY_LEXER, TERMS);
  buffer = nullptr;
  scanner = nullptr;
  ts = scan_tokenize(editor, S("@"), &buffer, &scanner);
  scan_scanner_set_recovery(scanner, true);
  while (scan_stream_next(ts)) {
  }
  check("recovery mode leaves undeclared failures invalid",
        scan_status(scanner) == SCAN_STATUS_INVALID);
  scan_token_stream_free(ts);
  scan_scanner_free(scanner);
  core_buffer_free(buffer);
  parse_grammar_free(editor);

  core_string_t grammar_text =
      S("@lex-number(digits(\"0123456789\"), sep(\"_\"), "
        "int(\"INTEGER\"))\n"
        "@lex-ident(as(\"IDENTIFIER\"))\n"
        "@lex-recover(\"INTEGER\", as(\"ERROR\"), sync(\";\"))\n"
        "@lex-fallback(\"reject\")\n"
        "<file> ::= %ERROR %\";\" %IDENTIFIER\n");
  parse_grammar_t *declared = parse_grammar_new();
  parse_grammar_set_error_recovery(declared, false);
  parse_bnf_declare_terminals(declared, grammar_text);
  bool grammar_loaded = load_test_grammar(grammar_text, S("file"), declared);

  buffer = nullptr;
  scanner = nullptr;
  source = S("1__oops; next");
  ts = grammar_loaded ? scan_tokenize(declared, source, &buffer, &scanner)
                      : nullptr;
  if (scanner) {
    scan_scanner_set_recovery(scanner, true);
  }
  error = ts ? scan_stream_next(ts) : nullptr;
  while (ts && scan_stream_next(ts)) {
  }
  check("BNF lexer specifications accept declarative recovery recipes",
        grammar_loaded && error &&
            parse_token_tid(error) == parse_lookup_terminal(declared, S("ERROR")) &&
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
        !invalid_program &&
            core_string_find(scan_lex_compile_error(), S("cannot be empty")) >=
                0);
  scan_lex_free(invalid_program);
  parse_grammar_free(invalid);
}

static parse_grammar_t *number_form_grammar(void) {
  static const char G[] =
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
  core_string_t text = core_string_borrow(G, sizeof(G) - 1);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  if (!load_test_grammar(text, CORE_STRING_STATIC("file"), g)) {
    parse_grammar_free(g);
    g = nullptr;
  }
  return g;
}

static void test_self_contained_number_forms(void) {
  parse_grammar_t *g = number_form_grammar();

  check("number forms accept separators and exponents",
        source_status(g, "0x1f 1_000 1.5e+2") == SCAN_STATUS_COMPLETE);
  kinds(g, "number forms own their point and exponent policies",
        "0 1. .5 1e2 0x1 0x1p2 0x1.fp2 0b101",
        "INTEGER FLOAT FLOAT FLOAT INTEGER FLOAT FLOAT INTEGER");
  kinds(g, "suffixes belong to one number form", "1j 0x1", "IMAGINARY INTEGER");
  kinds(g, "a suffix without a token preserves the form's token kind",
        "1L 1.0L", "INTEGER FLOAT");
  check("a prefixed form requires one of its own digits",
        source_status(g, "0x") == SCAN_STATUS_INVALID);
  check("a separator must occur between digits",
        source_status(g, "1__2") == SCAN_STATUS_INVALID);
  check("an exponent marker requires exponent digits",
        source_status(g, "1e+") == SCAN_STATUS_INVALID);
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
        scan_stream_append(ts, S("1i")) && scan_stream_next(ts) == nullptr);
  check("a numeric suffix resumes through its compiled trie",
        scan_stream_append(ts, S("mag ")) && scan_stream_next(ts) != nullptr);
  scan_token_stream_free(ts);

  ts = scan_tokenize_incremental_owned(g);
  bool linear = true;
  for (size_t i = 0; i < 4096; i++) {
    linear = linear && scan_stream_append(ts, S("1")) &&
             scan_stream_next(ts) == nullptr;
  }
  linear = linear && scan_stream_append(ts, S(" ")) &&
           scan_stream_next(ts) != nullptr &&
           scan_work_bytes(ts->scanner) < 8192;
  check("one-byte numeric streaming remains linear", linear);
  scan_token_stream_free(ts);

  parse_grammar_free(g);
}

#define number_error(label, source, expected) \
  number_error_check((label), S(source), S(expected))

static void number_error_check(const char *label, core_string_t text,
                               core_string_t expected) {
  parse_grammar_t *g = parse_grammar_new();

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = load_test_grammar(text, CORE_STRING_STATIC("file"), g);
  scan_lex_t *lex = loaded ? scan_grammar_lex_compiled(g) : nullptr;
  core_string_t error = scan_lex_build_error();

  check(label, loaded && !lex && core_string_find(error, expected) >= 0);
  parse_grammar_free(g);
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
  static const char G[] =
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
  core_string_t text = core_string_borrow(G, sizeof(G) - 1);
  parse_grammar_t *g = parse_grammar_new();

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = load_test_grammar(text, S("file"), g);
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
  core_string_t source;
  bool parsed;
} soft_keyword_parse_t;

static void *parse_soft_keyword_start(void *arg) {
  soft_keyword_parse_t *run = arg;
  parse_token_stream_t *ts = scan_tokenize_owned(run->grammar, run->source);
  parse_pwz_parser_t *parser =
      parse_pwz_new_for_start(run->grammar, run->start);

  run->parsed = parser && ts && parse_pwz_parse(parser, ts);
  parse_pwz_free(parser);
  scan_token_stream_free(ts);
  return nullptr;
}

static void test_soft_keywords(void) {
  static const char G[] =
      "@lex-ident(as(\"IDENTIFIER\"))\n"
      "@lex-soft-keywords(\"match\", \"case\")\n"
      "@lex-fallback(\"reject\")\n"
      "<statement> ::= %\"match\" %IDENTIFIER %\"case\"\n"
      "<name> ::= %IDENTIFIER\n";
  core_string_t text = core_string_borrow(G, sizeof(G) - 1);
  parse_grammar_t *g = parse_grammar_new();

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  bool loaded = load_test_grammar(text, S("statement"), g);
  check("soft-keyword declarations construct a lexer", loaded);

  if (loaded) {
    parse_token_stream_t *tokens =
        scan_tokenize_owned(g, S("match case ordinary"));
    parse_token_info_t *match = scan_stream_next(tokens);
    parse_token_info_t *case_word = scan_stream_next(tokens);
    parse_token_info_t *ordinary = scan_stream_next(tokens);

    check("soft keywords retain the ordinary identifier token kind",
          match && case_word && ordinary &&
              parse_token_tid(match) == PARSE_TOK_IDENTIFIER &&
              parse_token_tid(case_word) == PARSE_TOK_IDENTIFIER &&
              parse_token_tid(ordinary) == PARSE_TOK_IDENTIFIER);
    scan_token_stream_free(tokens);

    parse_nonterm_t *statement = parse_find_nonterm(g, S("statement"));
    parse_nonterm_t *name = parse_find_nonterm(g, S("name"));
    soft_keyword_parse_t runs[] = {
        {.grammar = g,
         .start = parse_nonterm_id(statement),
         .source = S("match subject case")},
        {.grammar = g, .start = parse_nonterm_id(name), .source = S("match")},
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
        .source = S("other subject case"),
    };
    parse_soft_keyword_start(&wrong);
    check("contextual terminals still require their declared spelling",
          !wrong.parsed);

    parse_token_stream_t *ts = scan_tokenize_owned(g, S("match subject case"));
    parse_pwz_parser_t *parser =
        parse_pwz_new_for_start(g, parse_nonterm_id(statement));
    bool parsed = parser && parse_pwz_parse(parser, ts);
    parse_token_info_t *first =
        parsed ? parse_tree_find_first_terminal(parse_pwz_get_tree(parser))
               : nullptr;
    check("parse trees preserve the original soft-keyword token",
          first && parse_token_tid(first) == PARSE_TOK_IDENTIFIER &&
              parse_token_has_value(first) &&
              core_string_eq(core_option_get(parse_token_value(first)), S("match")));
    parse_pwz_free(parser);
    scan_token_stream_free(ts);
  }

  parse_grammar_free(g);

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
  static const core_string_t PY_TERMS[] = {
      S("IDENTIFIER"),
      S("STRING"),
      {0},
  };
  static const core_string_t JS_TERMS[] = {
      S("IDENTIFIER"),      S("STRING"),        S("TEMPLATE_HEAD"),
      S("TEMPLATE_MIDDLE"), S("TEMPLATE_TAIL"), {0},
  };
  parse_grammar_t *py = mk(&PYTHON, PY_TERMS);
  parse_grammar_t *js = mk(&JS, JS_TERMS);

  check("a single-line string at EOF is incomplete",
        source_status(py, "\"unfinished") == SCAN_STATUS_INCOMPLETE);
  check("a newline in a single-line string is invalid",
        source_status(py, "\"bad\n") == SCAN_STATUS_INVALID);
  check("an interpolated string tail at EOF is incomplete",
        source_status(js, "`a${x}") == SCAN_STATUS_INCOMPLETE);
  parse_token_stream_t *stream = scan_tokenize_owned(js, S("\"open"));
  while (scan_stream_next(stream)) {
  }
  scan_diagnostic_t diagnostic = {0};
  check("incomplete scanner diagnostics remain inspectable",
        scan_diagnostic(stream->scanner, &diagnostic) &&
            diagnostic.status == SCAN_STATUS_INCOMPLETE &&
            diagnostic.problem.kind == SCAN_DIAG_STRING_UNTERMINATED &&
            diagnostic.start_byte == 0 &&
            diagnostic.end_byte == 5);
  scan_token_stream_free(stream);

  parse_grammar_free(py);
  parse_grammar_free(js);
}

static void test_reject_fallback_accepts_declared_codepoints_only(void) {
  static const char G[] = "@lex-fallback(\"reject\")\n"
                               "<file> ::= %\"+\"\n";
  core_string_t text = core_string_borrow(G, sizeof(G) - 1);
  parse_grammar_t *g = parse_grammar_new();

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  check("a rejecting fallback grammar loads",
        load_test_grammar(text, CORE_STRING_STATIC("file"), g));

  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize(g, S("+@"), &buf, &sc);
  parse_token_info_t *declared = scan_stream_next(ts);
  parse_token_info_t *unknown = scan_stream_next(ts);

  check("reject fallback retains declared codepoint terminals",
        declared && parse_token_tid(declared) == '+');
  check("reject fallback emits progress for an undeclared codepoint",
        unknown && parse_token_tid(unknown) == PARSE_TOK_OTHER);
  check("reject fallback reports invalid input",
        scan_status(sc) == SCAN_STATUS_INVALID && sc->status_start_byte == 1);

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);
}

static parse_grammar_t *unicode_identifier_grammar(core_string_t version) {
  core_buffer_t *source = core_buffer_empty();

  core_buffer_printf(
      source,
      "@lex-ident-profile(\"safe\"%s%.*s%s, normalize(\"nfc\"), "
      "scripts(\"Latin\", \"Greek\"), mixed_scripts(\"diagnose\"), "
      "confusables(\"allow\"), emoji(\"diagnose\"))\n"
      "@lex-ident(as(\"IDENTIFIER\"), profile(\"safe\"))\n"
      "<file> ::= %%IDENTIFIER\n",
      version.data ? ", unicode(\"" : "", (int)version.u8_bytes,
      version.data ? version.data : "", version.data ? "\")" : "");
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_borrow(source->data, source->byte_len);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  if (!load_test_grammar(text, CORE_STRING_STATIC("file"), g)) {
    parse_grammar_free(g);
    g = nullptr;
  }
  core_buffer_free(source);
  return g;
}

static parse_grammar_t *unicode_confusable_grammar(void) {
  static const char G[] =
      "@lex-ident-profile(\"confusable\", "
      "scripts(\"Latin\", \"Cyrillic\"), "
      "mixed_scripts(\"allow\"), confusables(\"diagnose\"))\n"
      "@lex-ident(as(\"IDENTIFIER\"), profile(\"confusable\"))\n"
      "<file> ::= %IDENTIFIER\n";
  parse_grammar_t *g = parse_grammar_new();
  core_string_t text = core_string_borrow(G, sizeof(G) - 1);

  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, text);
  if (!load_test_grammar(text, CORE_STRING_STATIC("file"), g)) {
    parse_grammar_free(g);
    g = nullptr;
  }
  return g;
}

static void test_unicode_identifier_profiles(void) {
  core_string_t version = scan_lex_unicode_version();
  parse_grammar_t *g = unicode_identifier_grammar(version);
  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize(g, S("e\xCC\x81 "), &buf, &sc);
  parse_token_info_t *tok = scan_stream_next(ts);
  core_string_t value = {0};
  check("an identifier not in NFC is rejected with its composed spelling",
        scan_status(sc) == SCAN_STATUS_INVALID && sc->status_start_byte == 0 &&
            sc->status_end_byte == 3 &&
            sc->status_problem.kind == SCAN_DIAG_IDENTIFIER_NOT_NFC &&
            core_string_eq(sc->status_problem.detail, S("\xC3\xA9")));
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  ts = scan_tokenize_incremental_owned(g);
  bool resumed =
      scan_stream_append(ts, S("caf")) && scan_stream_next(ts) == nullptr &&
      scan_stream_append(ts, S("\xC3")) && scan_stream_next(ts) == nullptr &&
      scan_stream_append(ts, S("\xA9 "));
  tok = resumed ? scan_stream_next(ts) : nullptr;
  value = tok && parse_token_has_value(tok)
              ? core_option_get(parse_token_value(tok))
              : (core_string_t){0};
  check("Unicode identifiers resume across one-byte UTF-8 chunks",
        tok && value.u8_bytes == 5 && !memcmp(value.data, "caf\xC3\xA9", 5));
  scan_token_stream_free(ts);

  buf = nullptr;
  sc = nullptr;
  ts = scan_tokenize(g, S("a\xCE\xB1"), &buf, &sc);
  scan_stream_next(ts);
  check("mixed-script diagnostics identify the introducing scalar",
        scan_status(sc) == SCAN_STATUS_INVALID && sc->status_start_byte == 1 &&
            sc->status_end_byte == 3);
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  buf = nullptr;
  sc = nullptr;
  ts = scan_tokenize(g, S("\xD0\xB0"), &buf, &sc);
  scan_stream_next(ts);
  check("script restrictions report a narrow source span",
        scan_status(sc) == SCAN_STATUS_INVALID && sc->status_start_byte == 0 &&
            sc->status_end_byte == 2);
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  buf = nullptr;
  sc = nullptr;
  ts = scan_tokenize(g, S("\xF0\x9F\x8E\x89"), &buf, &sc);
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
  ts = scan_tokenize(g, S("\xD0\xB0"), &buf, &sc);
  scan_stream_next(ts);
  check("confusable checking is an optional narrow diagnostic",
        scan_status(sc) == SCAN_STATUS_INVALID && sc->status_start_byte == 0 &&
            sc->status_end_byte == 2);
  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);
  parse_grammar_free(g);

  g = unicode_identifier_grammar(S("0.0"));
  check("identifier profiles reject unavailable Unicode versions",
        g && !scan_grammar_lex_compiled(g) &&
            core_string_find(scan_lex_build_error(), S("requests version")) >=
                0);
  parse_grammar_free(g);

  core_string_t bad_text =
      S("@lex-ident-profile(\"bad\", scripts(\"NotAScript\"))\n"
        "@lex-ident(as(\"IDENTIFIER\"), profile(\"bad\"))\n"
        "<file> ::= %IDENTIFIER\n");
  g = parse_grammar_new();
  parse_grammar_set_error_recovery(g, false);
  parse_bnf_declare_terminals(g, bad_text);
  bool loaded = load_test_grammar(bad_text, CORE_STRING_STATIC("file"), g);
  check("identifier profiles reject unknown script names at construction",
        loaded && !scan_grammar_lex_compiled(g) &&
            core_string_find(scan_lex_build_error(), S("unknown script")) >= 0);
  parse_grammar_free(g);
}

typedef struct {
  parse_grammar_t *grammar;
  core_string_t source;
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
    parse_token_stream_t *ts =
        scan_tokenize(run->grammar, run->source, &buf, &sc);
    size_t count = 0;

    parse_token_info_t *token = nullptr;
    while ((token = scan_stream_next(ts))) {
      count++;
      if ((run->expected_token.data &&
           parse_token_tid(token) != parse_terminal_id(run->grammar,
                                           run->expected_token)) ||
          (run->expected_value.data &&
           (!parse_token_has_value(token) ||
            !core_string_eq(core_option_get(parse_token_value(token)),
                            run->expected_value))) ||
          (run->expected_type.data &&
           (!core_option_is_set(parse_token_literal_type(token)) ||
            !core_string_eq(core_option_get(parse_token_literal_type(token)),
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
      {.grammar = g, .source = S("[=[alpha]=]")},
      {.grammar = g, .source = S("[==ffi[beta]==]")},
      {.grammar = g, .source = S("[===bnf[gamma]===]")},
      {.grammar = g, .source = S("[=json[delta]=]")},
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

  parse_grammar_t *embedded = embedded_grammar(S("json"), true);
  concurrent_lex_t embedded_runs[] = {
      {.grammar = embedded, .source = S("a {{1 \"x\"}} b"), .expected_tokens = 6},
      {.grammar = embedded, .source = S("c {{2 \"y\"}} d"), .expected_tokens = 6},
      {.grammar = embedded, .source = S("e {{3 \"z\"}} f"), .expected_tokens = 6},
      {.grammar = embedded, .source = S("g {{4 \"w\"}} h"), .expected_tokens = 6},
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
       .source = S("\"parallel\""),
       .expected_token = S("SPECIAL"),
       .expected_value = S("decoded"),
       .expected_type = S("duration")},
      {.grammar = hooked,
       .source = S("\"parallel\""),
       .expected_token = S("SPECIAL"),
       .expected_value = S("decoded"),
       .expected_type = S("duration")},
      {.grammar = hooked,
       .source = S("\"parallel\""),
       .expected_token = S("SPECIAL"),
       .expected_value = S("decoded"),
       .expected_type = S("duration")},
      {.grammar = hooked,
       .source = S("\"parallel\""),
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
        {.grammar = unicode, .source = S("abc")},
        {.grammar = unicode, .source = S("abc")},
        {.grammar = unicode, .source = S("abc")},
        {.grammar = unicode, .source = S("abc")},
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
  core_string_t shadowed_text =
      S("@lex-string(\"/\", \"/\", as(\"STRING\"))\n"
        "<file> ::= %STRING | %\"/\"\n");
  parse_grammar_t *shadowed = parse_grammar_new();
  parse_grammar_set_error_recovery(shadowed, false);
  parse_bnf_declare_terminals(shadowed, shadowed_text);
  bool shadowed_loaded = load_test_grammar(shadowed_text, S("file"), shadowed);
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
  static const core_string_t prefix_terms[] = {S("REGEX"), {0}};
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
  static const core_string_t sigil_terms[] = {S("SHORT"), S("LONG"), {0}};
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
        !lex && core_string_find(scan_lex_compile_error(),
                                 S("unknown token 'NOT_DECLARED'")) >= 0);
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
      !lex && core_string_find(scan_lex_compile_error(),
                               S("duplicate output token 'SPECIAL'")) >= 0);
  parse_grammar_free(g);

  static const core_string_t missing_operator[] = {S("missing-op"), {}};
  static const scan_lex_spec_t bad_operator = {
      .name = S("analysis-bad-operator"),
      .operators = missing_operator,
  };
  g = parse_grammar_new();
  lex = scan_lex_compile(&bad_operator, g);
  check("programmatic operators must name grammar terminals",
        !lex &&
            core_string_find(scan_lex_compile_error(), S("unknown token")) >= 0);
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
  core_string_t first_error = {0};
  if (!lex) {
    first_error = core_string_dup(scan_lex_compile_error());
  }
  lex = scan_lex_compile(&second, g);
  check("conflict diagnostics are independent of declaration order",
        !lex && first_error.data &&
            core_string_eq(first_error, scan_lex_compile_error()) &&
            core_string_find(first_error, S("a.lex")) >= 0 &&
            core_string_find(first_error, S("z.lex")) >= 0);
  core_free(first_error.data);
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
  static const core_string_t output_terms[] = {
      S("IDENTIFIER"),
      S("STRING"),
      {0},
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

  scan_lex_free(lex);
  parse_grammar_free(g);

  enum { MANY = 8192 };
  scan_lex_sigil_t *many = core_alloc_array(scan_lex_sigil_t, MANY + 1);
  scan_lex_declaration_t *many_origins =
      core_alloc_array(scan_lex_declaration_t, MANY + 1);
  core_buffer_t *spellings = core_buffer_empty();
  for (size_t i = 0; i < MANY; i++) {
    core_buffer_printf(spellings, "p%08zu", i);
  }
  for (size_t i = 0; i < MANY; i++) {
    core_string_t spelling = core_string_slice(core_buffer_string(spellings),
                                               i * 9, 9);
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
  core_buffer_free(spellings);
  core_free(many_origins);
  core_free(many);
}


int main(void) {
  scan_lex_register(&JSON);
  test_indentation_becomes_tokens();
  test_newline_each_line();
  test_layout_tokens_ignore_only_when_unmatched();
  test_indentation_policies();
  test_incremental_indentation_waits_for_content();
  test_embedded_lexer_switching();
  test_interpolation_pushes_a_mode();
  test_templates_and_regex();
  test_sigils_and_heredocs();
  test_attached_postfix_sigil();
  test_conditional_attached_sigil();
  test_heredoc_families();
  test_literal_postprocessing_hooks();
  test_a_grammar_declares_its_own_lexis();
  test_comment_region_anchors_are_trivia();
  test_string_families_expand_once();
  test_string_families_diagnose_invalid_declarations();
  test_paired_string_families_nest();
  test_composable_string_escape_profiles();
  test_literal_string_escape_prefixes();
  test_large_string_family_dispatch_is_bounded();
  test_large_number_suffix_dispatch_is_bounded();
  test_long_numeric_trie_paths_resume_linearly();
  test_an_unknown_option_is_named();
  test_lex_declarations_have_no_fixed_limit();
  test_zero_minimum_hash_fences();
  test_symmetric_fences_preserve_source_spans();
  test_fence_padding_requires_both_boundaries();
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
  test_terminals_have_no_byte_width_limit();
  test_grammar_finalization_is_thread_safe();
  test_malformed_token_recovery();
  test_self_contained_number_forms();
  test_number_forms_diagnose_invalid_declarations();
  test_reusable_lexical_packages();
  test_soft_keywords();
  test_strings_distinguish_incomplete_from_invalid();
  test_reject_fallback_accepts_declared_codepoints_only();
  test_unicode_identifier_profiles();
  test_lexer_spec_conflict_analysis();
  test_compiled_lexers_are_safe_to_share();

  return report("scanner lex");
}
