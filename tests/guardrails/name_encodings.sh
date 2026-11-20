fixture names
: > "$work/names/tests/name-encoding-baseline"
printf 'void f(char *b, unsigned n) {\n    snprintf(b, 32, "%%s #%%u", "x", n);\n}\n' > "$work/names/compiler/a.c"
expect pass "names: display text" check name_encodings names
printf 'void g(char *b, unsigned n) {\n    snprintf(b, 32, "%%s#%%u", "x", n);\n}\n' >> "$work/names/compiler/a.c"
expect fail "names: sigil-number identity" check name_encodings names
