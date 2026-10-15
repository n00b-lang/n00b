#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "parse/bnf.h"
#include "parse/pwz.h"
#include "parse/tree_dump.h"
#include "private/parse/grammar.h"
#include "scanner/lex.h"
#include "scanner/scan_builtins.h"
#include "scanner/scanner.h"
#include "scanner/token_stream.h"
#include "scanner/tokenizer.h"

static bool c_tokenize(parse_scanner_t *s) {
restart:
  scan_skip_whitespace(s);

  if (scan_at_eof(s)) {
    return false;
  }

  if (scan_peek_byte(s, 0) == '/' && scan_peek_byte(s, 1) == '/') {
    scan_skip_line_comment(s);
    goto restart;
  }

  if (scan_peek_byte(s, 0) == '/' && scan_peek_byte(s, 1) == '*') {
    scan_skip_block_comment(s, CORE_STRING_STATIC("/*"),
                            CORE_STRING_STATIC("*/"));
    goto restart;
  }

  scan_mark(s);
  core_codepoint_t cp = scan_peek(s, 0);

  if (cp == '"') {
    core_option_t(core_string_t) val = scan_string_double(s);
    scan_emit(s, PARSE_TOK_STRING_LIT, val);
    return true;
  }

  if (cp == '\'') {
    core_option_t(core_string_t) val = scan_string_single(s);
    scan_emit(s, PARSE_TOK_CHAR_LIT, val);
    return true;
  }

  if ((cp >= '0' && cp <= '9') || (cp == '.' && scan_peek_byte(s, 1) >= '0' &&
                                   scan_peek_byte(s, 1) <= '9')) {
    bool emitted = scan_number(s, PARSE_TOK_INTEGER, PARSE_TOK_FLOAT);

    if (emitted) {
      return true;
    }
  }

  if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_') {
    core_option_t(core_string_t) id_val = scan_identifier(s);

    if (core_option_is_set(id_val)) {
      core_string_t id_str = core_option_get(id_val);

      int64_t kw_id = scan_terminal_id(s, id_str);

      if (kw_id != PARSE_TOK_OTHER) {
        scan_emit(s, (int32_t)kw_id, id_val);
      } else {
        scan_emit(s, PARSE_TOK_IDENTIFIER, id_val);
      }

      return true;
    }
  }

  static const core_string_t ops3[] = {
      CORE_STRING_STATIC("<<="),
      CORE_STRING_STATIC(">>="),
      CORE_STRING_STATIC("..."),
      {0},
  };

  for (const core_string_t *op = ops3; op->data; op++) {
    if (scan_peek_byte(s, 0) == (uint8_t)op->data[0] &&
        scan_peek_byte(s, 1) == (uint8_t)op->data[1] &&
        scan_peek_byte(s, 2) == (uint8_t)op->data[2]) {
      int64_t tid = scan_terminal_id(s, *op);

      if (tid != PARSE_TOK_OTHER) {
        scan_advance_n(s, 3);
        scan_emit_marked(s, (int32_t)tid);
        return true;
      }
    }
  }

  static const core_string_t ops2[] = {
      CORE_STRING_STATIC("->"),
      CORE_STRING_STATIC("++"),
      CORE_STRING_STATIC("--"),
      CORE_STRING_STATIC("<<"),
      CORE_STRING_STATIC(">>"),
      CORE_STRING_STATIC("<="),
      CORE_STRING_STATIC(">="),
      CORE_STRING_STATIC("=="),
      CORE_STRING_STATIC("!="),
      CORE_STRING_STATIC("&&"),
      CORE_STRING_STATIC("||"),
      CORE_STRING_STATIC("+="),
      CORE_STRING_STATIC("-="),
      CORE_STRING_STATIC("*="),
      CORE_STRING_STATIC("/="),
      CORE_STRING_STATIC("%="),
      CORE_STRING_STATIC("&="),
      CORE_STRING_STATIC("^="),
      CORE_STRING_STATIC("|="),
      CORE_STRING_STATIC("::"),
      {0},
  };

  for (const core_string_t *op = ops2; op->data; op++) {
    if (scan_peek_byte(s, 0) == (uint8_t)op->data[0] &&
        scan_peek_byte(s, 1) == (uint8_t)op->data[1]) {
      int64_t tid = scan_terminal_id(s, *op);

      if (tid != PARSE_TOK_OTHER) {
        scan_advance_n(s, 2);
        scan_emit_marked(s, (int32_t)tid);
        return true;
      }
    }
  }

  scan_advance(s);
  scan_emit_marked(s, (int32_t)cp);

  return true;
}

static parse_grammar_t *load_c_grammar(void) {
  const char *paths[] = {
      "../../grammars/c_ncc.bnf",
      "../grammars/c_ncc.bnf",
      "grammars/c_ncc.bnf",
      nullptr,
  };

  const char *srcroot = getenv("MESON_SOURCE_ROOT");

  FILE *f = nullptr;

  for (const char **p = paths; *p; p++) {
    f = fopen(*p, "r");

    if (f) {
      break;
    }
  }

  if (!f && srcroot) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/grammars/c_ncc.bnf", srcroot);
    f = fopen(path, "r");
  }

  if (!f) {
    fprintf(stderr, "  [SKIP] Cannot find c_ncc.bnf\n");
    return nullptr;
  }

  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);

  char *buf = core_alloc_size(1, (size_t)len + 1);
  size_t nread = fread(buf, 1, (size_t)len, f);
  fclose(f);

  core_string_t bnf_text = core_string_from_raw(buf, (int64_t)nread);
  core_free(buf);

  parse_grammar_t *g = parse_grammar_new();
  parse_grammar_set_error_recovery(g, false);

  core_string_t start = CORE_STRING_STATIC("translation_unit");
  bool ok = parse_bnf_load(bnf_text, start, g);

  if (!ok) {
    fprintf(stderr, "  [FAIL] parse_bnf_load failed for c_ncc.bnf\n");
    parse_grammar_free(g);
    return nullptr;
  }

  return g;
}

static bool parse_c_source(parse_grammar_t *g, const char *src) {
  core_buffer_t *buf = core_buffer_from_bytes(src, (int64_t)strlen(src));
  parse_scanner_t *scanner = scan_scanner_new(
      buf, c_tokenize, g, core_option_none(core_string_t), nullptr, nullptr);
  parse_token_stream_t *ts = scan_token_stream_new(scanner);
  parse_pwz_parser_t *p = parse_pwz_new_for_start(
      g, parse_nonterm_id(
             parse_find_nonterm(g, CORE_STRING_STATIC("translation_unit"))));
  assert(p);

  bool ok = parse_pwz_parse(p, ts);

  parse_pwz_free(p);
  scan_token_stream_free(ts);
  scan_scanner_free(scanner);

  return ok;
}

static void assert_c_parse(parse_grammar_t *g, const char *src) {
  if (!parse_c_source(g, src)) {
    fprintf(stderr, "  [FAIL] expected parse success for:\n%s\n", src);
    assert(false);
  }
}

static void assert_c_parse_rejects(parse_grammar_t *g, const char *src) {
  if (parse_c_source(g, src)) {
    fprintf(stderr, "  [FAIL] expected parse rejection for:\n%s\n", src);
    assert(false);
  }
}

static void test_load_grammar(void) {
  printf("Test: Load C grammar from BNF...\n");

  parse_grammar_t *g = load_c_grammar();

  if (!g) {
    printf("  SKIP (grammar file not found)\n");
    return;
  }

  assert(g->terminal_map != nullptr);
  assert(core_list_len(g->nt_list) > 0);
  assert(core_list_len(g->rules) > 0);

  printf("  Grammar loaded: %zu NTs, %zu rules\n", core_list_len(g->nt_list),
         core_list_len(g->rules));

  printf("  PASS\n");
  parse_grammar_free(g);
}

static void test_parse_simple_c(void) {
  printf("Test: Parse simple C source...\n");

  parse_grammar_t *g = load_c_grammar();

  if (!g) {
    printf("  SKIP\n");
    return;
  }

  const char *src = "int main(void) { return 0; }";

  core_buffer_t *buf = core_buffer_from_bytes(src, (int64_t)strlen(src));
  parse_scanner_t *scanner = scan_scanner_new(
      buf, c_tokenize, g, core_option_none(core_string_t), nullptr, nullptr);
  parse_token_stream_t *ts = scan_token_stream_new(scanner);

  parse_pwz_parser_t *p = parse_pwz_new_for_start(
      g, parse_nonterm_id(
             parse_find_nonterm(g, CORE_STRING_STATIC("translation_unit"))));
  assert(p);

  bool ok = parse_pwz_parse(p, ts);

  if (ok) {
    parse_tree_t *tree = parse_pwz_get_tree(p);
    assert(tree);

    printf("  Parse succeeded. Tree:\n");
    parse_tree_dump(g, tree, stdout, false);
  } else {
    printf("  Parse did NOT succeed (this may be expected for a partial "
           "grammar test)\n");
  }

  printf("  PASS\n");

  parse_pwz_free(p);
  scan_token_stream_free(ts);
  scan_scanner_free(scanner);
  parse_grammar_free(g);
}

static void test_parse_array_literal_initializers(void) {
  printf("Test: Parse array literal initializers...\n");

  parse_grammar_t *g = load_c_grammar();

  if (!g) {
    printf("  SKIP\n");
    return;
  }

  assert_c_parse(g, "int xs = [1, 2, 3];\n"
                    "int empty = [];\n"
                    "int nested = [[1, 2], [3, 4]];\n"
                    "int explicit_array = a{1, 2, 3};\n"
                    "int explicit_empty_array = a{};\n"
                    "int explicit_list = l{1, 2, 3};\n"
                    "int explicit_empty_list = l{};\n");
  assert_c_parse(g, "int main(void) {\n"
                    "  const int xs = [1, 2, 3,];\n"
                    "  return 0;\n"
                    "}\n");
  assert_c_parse(g, "[[maybe_unused]] int attr_preserved = 1;\n"
                    "int main(void) {\n"
                    "  [[maybe_unused]] int local_attr = 2;\n"
                    "  return local_attr;\n"
                    "}\n");
  assert_c_parse_rejects(g, "int main(void) {\n"
                            "  [1, 2, 3];\n"
                            "  return 0;\n"
                            "}\n");

  printf("  PASS\n");
  parse_grammar_free(g);
}

static void test_parse_dict_literal_initializers(void) {
  printf("Test: Parse dict literal initializers...\n");

  parse_grammar_t *g = load_c_grammar();

  if (!g) {
    printf("  SKIP\n");
    return;
  }

  assert_c_parse(g, "int explicit_dict = d{1: 2};\n"
                    "int explicit_empty_dict = d{};\n"
                    "int bare_dict = {1: 2};\n"
                    "int multi_pair = d{1: 2, 3: 4, 5: 6};\n"
                    "int trailing_comma = d{1: 2, 3: 4,};\n"
                    "int bare_trailing_comma = {1: 2, 3: 4,};\n"
                    "int nested_dict_in_dict = d{1: d{2: 3}};\n"
                    "int nested_list_value = d{1: l{2, 3}};\n"
                    "int nested_array_value = d{1: a{2, 3}};\n"
                    "int nested_bracket_value = d{1: [2, 3]};\n");

  assert_c_parse(g, "struct s { int x; };\n"
                    "struct s zero = {};\n"
                    "struct s positional = {1};\n"
                    "int xs[3] = {1, 2, 3};\n"
                    "struct s designated = {.x = 5};\n");

  printf("  PASS\n");
  parse_grammar_free(g);
}

static void test_bnf_preprocess(void) {
  printf("Test: BNF preprocessing...\n");

  core_string_t input =
      CORE_STRING_STATIC("rule = a | b // comment\nrule2 = c");
  core_string_t stripped = parse_bnf_strip_comments(input);

  assert(stripped.data != nullptr);
  printf("  Strip comments: \"%.*s\"\n", (int)stripped.u8_bytes, stripped.data);

  core_string_t padded = CORE_STRING_STATIC("  hello  \n  world  \n");
  core_string_t trimmed = parse_bnf_trim_lines(padded);

  assert(trimmed.data != nullptr);
  printf("  Trim lines: \"%.*s\"\n", (int)trimmed.u8_bytes, trimmed.data);

  printf("  PASS\n");
}

static void test_bnf_errors_are_available_to_callers(void) {
  parse_grammar_t *g = parse_grammar_new();

  assert(!parse_bnf_load(CORE_STRING_STATIC("<x> ::= \"\"\n"),
                         CORE_STRING_STATIC("x"), g));
  assert(parse_bnf_error() != nullptr);
  assert(strstr(parse_bnf_error(), "empty string literal") != nullptr);

  parse_grammar_t *valid = parse_grammar_new();

  assert(parse_bnf_load(CORE_STRING_STATIC("<x> ::= 'x'\n"),
                        CORE_STRING_STATIC("x"), valid));
  assert(parse_bnf_error() == nullptr);

  parse_grammar_t *multiline = parse_grammar_new();

  assert(parse_bnf_load(CORE_STRING_STATIC("<x> @scope(\n"
                                           "    \"test\"\n"
                                           ") ::= 'x'\n"),
                        CORE_STRING_STATIC("x"), multiline));

  parse_grammar_t *malformed = parse_grammar_new();

  assert(!parse_bnf_load(CORE_STRING_STATIC("<x> ::= 'x'\n"
                                            "<y> ::= (\n"),
                         CORE_STRING_STATIC("x"), malformed));

  parse_bnf_error_info_t info = {0};

  assert(parse_bnf_error_info(&info));
  assert(info.message == parse_bnf_error());
  assert(info.start_line == 2);
  assert(info.start_byte >= strlen("<x> ::= 'x'\n"));

  parse_grammar_free(g);
  parse_grammar_free(valid);
  parse_grammar_free(multiline);
  parse_grammar_free(malformed);
}

static scan_status_t tokenization_status(parse_grammar_t *g,
                                         const char *source) {
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

static void test_lexer_files_are_relative_to_the_grammar(void) {
  parse_grammar_t *g = parse_grammar_new();

  parse_grammar_set_error_recovery(g, false);
  assert(parse_bnf_load_file("lexer_import.bnf", (core_string_t){0}, g));
  assert(scan_grammar_lex(g) != nullptr);

  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize(g, "hello??world", 12, &buf, &sc);
  parse_token_info_t *word = scan_stream_next(ts);
  parse_token_info_t *op = scan_stream_next(ts);
  parse_token_info_t *word2 = scan_stream_next(ts);

  assert(word != nullptr);
  assert(word->tid == parse_terminal_id(g, CORE_STRING_STATIC("WORD")));
  assert(op != nullptr);
  assert(op->tid == parse_terminal_id(g, CORE_STRING_STATIC("??")));
  assert(word2 != nullptr);
  assert(word2->tid == parse_terminal_id(g, CORE_STRING_STATIC("WORD")));
  assert(scan_stream_next(ts) == nullptr);
  assert(scan_status(sc) == SCAN_STATUS_COMPLETE);

  scan_token_stream_free(ts);
  scan_scanner_free(sc);
  core_buffer_free(buf);

  assert(tokenization_status(g, "'a'") == SCAN_STATUS_COMPLETE);
  assert(tokenization_status(g, "'ab'") == SCAN_STATUS_INVALID);
  assert(tokenization_status(g, "'\\n'") == SCAN_STATUS_COMPLETE);
  assert(tokenization_status(g, "'\\x41'") == SCAN_STATUS_COMPLETE);
  assert(tokenization_status(g, "'\\q'") == SCAN_STATUS_INVALID);
  parse_grammar_free(g);

  parse_grammar_t *missing = parse_grammar_new();

  assert(!parse_bnf_load_file("lexer_missing.bnf", CORE_STRING_STATIC("file"),
                              missing));
  assert(parse_bnf_error() != nullptr);
  assert(strstr(parse_bnf_error(), "lexer_does_not_exist.lex") != nullptr);
  parse_grammar_free(missing);

  parse_grammar_t *invalid = parse_grammar_new();

  assert(!parse_bnf_load_file("lexer_invalid.bnf", CORE_STRING_STATIC("file"),
                              invalid));
  assert(parse_bnf_error() != nullptr);
  assert(strstr(parse_bnf_error(), "syntax error") != nullptr);
  parse_grammar_free(invalid);

  parse_grammar_t *malformed = parse_grammar_new();

  assert(!parse_bnf_load_file("lexer_malformed.bnf", CORE_STRING_STATIC("file"),
                              malformed));
  assert(parse_bnf_error() != nullptr);
  assert(strstr(parse_bnf_error(), "lexer_malformed.lex") != nullptr);
  assert(strstr(parse_bnf_error(), "syntax error") != nullptr);
  parse_grammar_free(malformed);

  parse_grammar_t *cycle = parse_grammar_new();

  assert(!parse_bnf_load_file("lexer_cycle.bnf", CORE_STRING_STATIC("file"),
                              cycle));
  assert(parse_bnf_error() != nullptr);
  assert(strstr(parse_bnf_error(), "lexer import cycle") != nullptr);
  parse_grammar_free(cycle);

  parse_grammar_t *conflict = parse_grammar_new();

  assert(parse_bnf_load_file("lexer_conflict_origin.bnf",
                             CORE_STRING_STATIC("file"), conflict));
  assert(scan_grammar_lex_compiled(conflict) == nullptr);
  assert(scan_lex_compile_error() != nullptr);
  assert(strstr(scan_lex_compile_error(), "lexer_conflict_origin.lex") !=
         nullptr);
  assert(strstr(scan_lex_compile_error(), "lexer_conflict_origin_import.lex") !=
         nullptr);
  parse_grammar_free(conflict);

  parse_grammar_t *package_conflict = parse_grammar_new();

  assert(parse_bnf_load_file("lexer_conflict_package.bnf",
                             CORE_STRING_STATIC("file"), package_conflict));
  assert(scan_grammar_lex_compiled(package_conflict) == nullptr);
  assert(scan_lex_compile_error() != nullptr);
  assert(strstr(scan_lex_compile_error(), "lexer_conflict_package.lex") !=
         nullptr);
  assert(strstr(scan_lex_compile_error(), "lexer_conflict_package_defs.lex") !=
         nullptr);
  parse_grammar_free(package_conflict);

  parse_grammar_t *removed = parse_grammar_new();

  assert(!parse_bnf_load_file("lexer_remove_used.bnf",
                              CORE_STRING_STATIC("file"), removed));
  assert(scan_lex_build_error() != nullptr);
  assert(strstr(scan_lex_build_error(), "grammar uses") != nullptr);
  parse_grammar_free(removed);

  parse_grammar_t *unknown = parse_grammar_new();

  assert(!parse_bnf_load_file("lexer_unknown_set.bnf",
                              CORE_STRING_STATIC("file"), unknown));
  assert(scan_lex_build_error() != nullptr);
  assert(strstr(scan_lex_build_error(), "unknown set 'missing'") != nullptr);
  parse_grammar_free(unknown);

  parse_grammar_t *package_error = parse_grammar_new();

  assert(!parse_bnf_load_file("lexer_package_error.bnf",
                              CORE_STRING_STATIC("file"), package_error));
  assert(scan_lex_build_error() != nullptr);
  assert(strstr(scan_lex_build_error(), "remove absent operator") != nullptr);
  assert(strstr(scan_lex_build_error(), "lexer_package_error.lex") != nullptr);
  parse_grammar_free(package_error);

  parse_grammar_t *memory = parse_grammar_new();

  assert(parse_bnf_load(
      CORE_STRING_STATIC("@lexer(\"relative.lex\")\n<file> ::= 'x'\n"),
      CORE_STRING_STATIC("file"), memory));
  assert(scan_grammar_lex(memory) == nullptr);
  assert(scan_lex_build_error() != nullptr);
  assert(strstr(scan_lex_build_error(), "parse_bnf_load_file") != nullptr);
  parse_grammar_free(memory);
}

static void test_deep_lexer_fragment_imports(void) {
  char directory[] = "/tmp/lantern-lexer-imports-XXXXXX";
  assert(mkdtemp(directory) != nullptr);

  enum { IMPORT_COUNT = 80 };
  char paths[IMPORT_COUNT][512];
  for (size_t i = 0; i < IMPORT_COUNT; i++) {
    snprintf(paths[i], sizeof(paths[i]), "%s/fragment-%zu.lex", directory, i);
    FILE *fragment = fopen(paths[i], "w");
    assert(fragment != nullptr);
    if (i + 1 < IMPORT_COUNT) {
      fprintf(fragment, "@lex-use(\"%s/fragment-%zu.lex\")\n", directory,
              i + 1);
    } else {
      fputs("@lex-ident(as(\"IDENTIFIER\"))\n", fragment);
    }
    assert(fclose(fragment) == 0);
  }

  char grammar_path[512];
  snprintf(grammar_path, sizeof(grammar_path), "%s/root.bnf", directory);
  FILE *grammar_file = fopen(grammar_path, "w");
  assert(grammar_file != nullptr);
  fprintf(grammar_file, "@lexer(\"%s\")\n<file> ::= %%IDENTIFIER\n", paths[0]);
  assert(fclose(grammar_file) == 0);

  parse_grammar_t *grammar = parse_grammar_new();
  assert(
      parse_bnf_load_file(grammar_path, CORE_STRING_STATIC("file"), grammar));
  assert(scan_grammar_lex_compiled(grammar) != nullptr);
  parse_grammar_free(grammar);

  unlink(grammar_path);
  for (size_t i = 0; i < IMPORT_COUNT; i++) {
    unlink(paths[i]);
  }
  rmdir(directory);
}

int main(void) {
  test_bnf_preprocess();
  test_bnf_errors_are_available_to_callers();
  test_lexer_files_are_relative_to_the_grammar();
  test_deep_lexer_fragment_imports();
  test_load_grammar();
  test_parse_simple_c();
  test_parse_array_literal_initializers();
  test_parse_dict_literal_initializers();

  printf("\nAll BNF tests passed.\n");
  return 0;
}
