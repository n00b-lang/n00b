/*
 * WP-019 regression fixture — gcc/clang asm-label declarations.
 *
 * All three asm-label keyword spellings (`__asm`, `__asm__`, `asm`)
 * place a string literal between `(` and `)`. The C tokenizer
 * previously emitted the string literal under the token-type name
 * `STRING_LIT`, which the C grammar (grammars/c_ncc.bnf) does not
 * declare — it declares `%STRING`. The mismatch made the scanner
 * silently drop the string token, so any asm-label failed to parse
 * against the merged grammar. This fixture must parse cleanly.
 *
 * This is target input the audit engine PARSES, not n00b-audit
 * source — the n00b-api-guidelines do not apply to it.
 */
int x __asm("y");
int z __asm__("zz");
extern int v asm("vv");

/* Real-world shape: linker section markers (cf. src/slay/codegen.c). */
extern const unsigned char __start_marker[] __asm("section$start$__DATA$__m");
extern const unsigned char __stop_marker[] __asm__("section$end$__DATA$__m");
