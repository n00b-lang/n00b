/*
 * WP-019 regression fixture — plain C string and char literals.
 *
 * The same C-tokenizer literal-type-name bug that broke asm-labels
 * dropped EVERY string literal (token-type `STRING_LIT` vs the
 * grammar's `%STRING`) and every char literal (`CHAR_LIT` vs
 * `%CHAR`). This fixture exercises the bare string/char path so the
 * regression is caught even if asm-labels were special-cased.
 *
 * Target input the audit engine PARSES, not n00b-audit source.
 */
const char *greeting = "hello, world";
const char  initial  = 'A';
const char *empty    = "";
const char *escaped  = "tab\there";
