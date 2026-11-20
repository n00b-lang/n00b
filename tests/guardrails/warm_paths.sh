fixture warm
printf 'n00bb_cache_has_module 1\n' > "$work/warm/tests/warm-baseline"
printf 'void f(void) {\n    if (n00bb_cache_has_module(c, m)) g();\n}\n' > "$work/warm/compiler/a.c"
expect pass "warm paths: at the baseline" check warm_paths warm
printf 'void h(void) {\n    if (n00bb_cache_has_module(c, m)) g();\n}\n' >> "$work/warm/compiler/a.c"
expect fail "warm paths: one more warm-only use" check warm_paths warm
