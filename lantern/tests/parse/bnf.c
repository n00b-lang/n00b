#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/os.h"
#include "parse/bnf.h"
#include "parse/pwz.h"
#include "parse/tree_dump.h"
#include "private/parse/grammar.h"
#include "scanner/lex.h"
#include "scanner/scan_builtins.h"
#include "scanner/scanner.h"
#include "scanner/token_stream.h"
#include "scanner/tokenizer.h"

#define S(x) CORE_STRING_STATIC(x)

static bool has(core_string_t s, core_string_t needle) {
  return s.data && core_string_find(s, needle) >= 0;
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
  assert(parse_bnf_error().data != nullptr);
  assert(has(parse_bnf_error(), S("empty string literal")));

  parse_grammar_t *valid = parse_grammar_new();

  assert(parse_bnf_load(CORE_STRING_STATIC("<x> ::= 'x'\n"),
                        CORE_STRING_STATIC("x"), valid));
  assert(parse_bnf_error().data == nullptr);

  parse_grammar_t *multiline = parse_grammar_new();

  assert(parse_bnf_load(CORE_STRING_STATIC("<x> @scope(\n"
                                           "    \"test\"\n"
                                           ") ::= 'x'\n"),
                        CORE_STRING_STATIC("x"), multiline));

  parse_grammar_t *malformed = parse_grammar_new();

  assert(!parse_bnf_load(CORE_STRING_STATIC("<x> ::= 'x'\n"
                                            "<y> ::= (\n"),
                         CORE_STRING_STATIC("x"), malformed));
  assert(parse_bnf_error().data != nullptr);

  parse_grammar_free(g);
  parse_grammar_free(valid);
  parse_grammar_free(multiline);
  parse_grammar_free(malformed);
}

static scan_status_t tokenization_status_s(parse_grammar_t *g,
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

#define tokenization_status(g, src) tokenization_status_s((g), S(src))

static void test_lexer_files_are_relative_to_the_grammar(void) {
  parse_grammar_t *g = parse_grammar_new();

  parse_grammar_set_error_recovery(g, false);
  assert(parse_bnf_load_file(S("lexer_import.bnf"), (core_string_t){0}, g));
  assert(scan_grammar_lex(g) != nullptr);

  core_buffer_t *buf = nullptr;
  parse_scanner_t *sc = nullptr;
  parse_token_stream_t *ts = scan_tokenize(g, S("hello??world"), &buf, &sc);
  parse_token_info_t *word = scan_stream_next(ts);
  parse_token_info_t *op = scan_stream_next(ts);
  parse_token_info_t *word2 = scan_stream_next(ts);

  assert(word != nullptr);
  assert(parse_token_tid(word)
         == parse_terminal_id(g, CORE_STRING_STATIC("WORD")));
  assert(op != nullptr);
  assert(parse_token_tid(op) == parse_terminal_id(g, CORE_STRING_STATIC("??")));
  assert(word2 != nullptr);
  assert(parse_token_tid(word2)
         == parse_terminal_id(g, CORE_STRING_STATIC("WORD")));
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

  assert(!parse_bnf_load_file(S("lexer_missing.bnf"), CORE_STRING_STATIC("file"),
                              missing));
  assert(parse_bnf_error().data != nullptr);
  assert(has(parse_bnf_error(), S("lexer_does_not_exist.lex")));
  parse_grammar_free(missing);

  parse_grammar_t *invalid = parse_grammar_new();

  assert(!parse_bnf_load_file(S("lexer_invalid.bnf"), CORE_STRING_STATIC("file"),
                              invalid));
  assert(parse_bnf_error().data != nullptr);
  assert(has(parse_bnf_error(), S("syntax error")));
  parse_grammar_free(invalid);

  parse_grammar_t *malformed = parse_grammar_new();

  assert(!parse_bnf_load_file(S("lexer_malformed.bnf"),
                              CORE_STRING_STATIC("file"), malformed));
  assert(parse_bnf_error().data != nullptr);
  assert(has(parse_bnf_error(), S("lexer_malformed.lex")));
  assert(has(parse_bnf_error(), S("syntax error")));
  parse_grammar_free(malformed);

  parse_grammar_t *cycle = parse_grammar_new();

  assert(!parse_bnf_load_file(S("lexer_cycle.bnf"), CORE_STRING_STATIC("file"),
                              cycle));
  assert(parse_bnf_error().data != nullptr);
  assert(has(parse_bnf_error(), S("lexer import cycle")));
  parse_grammar_free(cycle);

  parse_grammar_t *conflict = parse_grammar_new();

  assert(parse_bnf_load_file(S("lexer_conflict_origin.bnf"),
                             CORE_STRING_STATIC("file"), conflict));
  assert(scan_grammar_lex_compiled(conflict) == nullptr);
  assert(scan_lex_compile_error().data != nullptr);
  assert(has(scan_lex_compile_error(), S("lexer_conflict_origin.lex")));
  assert(has(scan_lex_compile_error(), S("lexer_conflict_origin_import.lex")));
  parse_grammar_free(conflict);

  parse_grammar_t *package_conflict = parse_grammar_new();

  assert(parse_bnf_load_file(S("lexer_conflict_package.bnf"),
                             CORE_STRING_STATIC("file"), package_conflict));
  assert(scan_grammar_lex_compiled(package_conflict) == nullptr);
  assert(scan_lex_compile_error().data != nullptr);
  assert(has(scan_lex_compile_error(), S("lexer_conflict_package.lex")));
  assert(has(scan_lex_compile_error(), S("lexer_conflict_package_defs.lex")));
  parse_grammar_free(package_conflict);

  parse_grammar_t *removed = parse_grammar_new();

  assert(!parse_bnf_load_file(S("lexer_remove_used.bnf"),
                              CORE_STRING_STATIC("file"), removed));
  assert(scan_lex_build_error().data != nullptr);
  assert(has(scan_lex_build_error(), S("grammar uses")));
  parse_grammar_free(removed);

  parse_grammar_t *unknown = parse_grammar_new();

  assert(!parse_bnf_load_file(S("lexer_unknown_set.bnf"),
                              CORE_STRING_STATIC("file"), unknown));
  assert(scan_lex_build_error().data != nullptr);
  assert(has(scan_lex_build_error(), S("unknown set 'missing'")));
  parse_grammar_free(unknown);

  parse_grammar_t *package_error = parse_grammar_new();

  assert(!parse_bnf_load_file(S("lexer_package_error.bnf"),
                              CORE_STRING_STATIC("file"), package_error));
  assert(scan_lex_build_error().data != nullptr);
  assert(has(scan_lex_build_error(), S("remove absent operator")));
  assert(has(scan_lex_build_error(), S("lexer_package_error.lex")));
  parse_grammar_free(package_error);

  parse_grammar_t *memory = parse_grammar_new();

  assert(parse_bnf_load(
      CORE_STRING_STATIC("@lexer(\"relative.lex\")\n<file> ::= 'x'\n"),
      CORE_STRING_STATIC("file"), memory));
  assert(scan_grammar_lex(memory) == nullptr);
  assert(scan_lex_build_error().data != nullptr);
  assert(has(scan_lex_build_error(), S("parse_bnf_load_file")));
  parse_grammar_free(memory);
}

static void test_deep_lexer_fragment_imports(void) {
  core_string_t temp_root = core_os_temp_root();
  core_buffer_t stem = {0};
  core_buffer_append_string(&stem, temp_root);
  core_buffer_puts(&stem, "/lantern-lexer-imports-");
  core_string_t dir = core_os_make_unique_directory(core_buffer_string(&stem));
  core_buffer_release(&stem);
  core_free(temp_root.data);
  assert(dir.data != nullptr);

  enum { IMPORT_COUNT = 80 };
  char *paths[IMPORT_COUNT];
  size_t path_lens[IMPORT_COUNT];
  for (size_t i = 0; i < IMPORT_COUNT; i++) {
    core_buffer_t text = {0};
    core_buffer_printf(&text, "%.*s/fragment-%zu.lex", (int)dir.u8_bytes,
                       dir.data, i);
    path_lens[i] = text.byte_len;
    paths[i] = core_string_to_cstr(core_buffer_string(&text));
    core_buffer_release(&text);
    assert(paths[i] != nullptr);
    FILE *fragment = fopen(paths[i], "w");
    assert(fragment != nullptr);
    if (i + 1 < IMPORT_COUNT) {
      fprintf(fragment, "@lex-use(\"%.*s/fragment-%zu.lex\")\n",
              (int)dir.u8_bytes, dir.data, i + 1);
    } else {
      fputs("@lex-ident(as(\"IDENTIFIER\"))\n", fragment);
    }
    assert(fclose(fragment) == 0);
  }

  core_buffer_t grammar_text = {0};
  core_buffer_append_string(&grammar_text, dir);
  core_buffer_puts(&grammar_text, "/root.bnf");
  size_t grammar_len = grammar_text.byte_len;
  char *grammar_path = core_string_to_cstr(core_buffer_string(&grammar_text));
  core_buffer_release(&grammar_text);
  assert(grammar_path != nullptr);
  FILE *grammar_file = fopen(grammar_path, "w");
  assert(grammar_file != nullptr);
  fprintf(grammar_file, "@lexer(\"%.*s\")\n<file> ::= %%IDENTIFIER\n",
          (int)path_lens[0], paths[0]);
  assert(fclose(grammar_file) == 0);

  parse_grammar_t *grammar = parse_grammar_new();
  assert(parse_bnf_load_file(
      core_string_borrow(grammar_path, grammar_len),
      CORE_STRING_STATIC("file"), grammar));
  assert(scan_grammar_lex_compiled(grammar) != nullptr);
  parse_grammar_free(grammar);

  core_os_remove_file(core_string_borrow(grammar_path, grammar_len));
  core_free(grammar_path);
  for (size_t i = 0; i < IMPORT_COUNT; i++) {
    core_os_remove_file(core_string_borrow(paths[i], path_lens[i]));
    core_free(paths[i]);
  }
  core_os_remove_directory(dir);
  core_free(dir.data);
}

int main(void) {
  test_bnf_preprocess();
  test_bnf_errors_are_available_to_callers();
  test_lexer_files_are_relative_to_the_grammar();
  test_deep_lexer_fragment_imports();

  printf("\nAll BNF tests passed.\n");
  return 0;
}
