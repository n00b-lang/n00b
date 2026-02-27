#pragma once

// Token-pasting helpers for typeid() replacement.
#define _N00B_CAT2(a, b) a ## b
#define N00B_CONCAT(a, b) _N00B_CAT2(a, b)

// N00B_FIRST: select first argument (used for optional-arg macros).
#define _N00B_FIRST_IMPL(x, ...) x
#define N00B_FIRST(...) _N00B_FIRST_IMPL(__VA_ARGS__)

// typeid() replacement: produces a struct tag by token-pasting.
// ncc's typeid("n00b_list", T) becomes n00b_list_ ## T here.
#define typeid(...) _TYPEID_SEL(__VA_ARGS__, _TYPEID4, _TYPEID3, _TYPEID2, _TYPEID1)(__VA_ARGS__)
#define _TYPEID_SEL(_1,_2,_3,_4,N,...) N
#define _TYPEID1(a) a
#define _TYPEID2(a, b) N00B_CONCAT(a ## _, b)
#define _TYPEID3(a, b, c) N00B_CONCAT(N00B_CONCAT(a ## _, b), _ ## c)
#define _TYPEID4(a, b, c, d) N00B_CONCAT(N00B_CONCAT(N00B_CONCAT(a ## _, b), _ ## c), _ ## d)

// typehash() — unused at runtime, always 0.
#define typehash(...) ((uint64_t)0)

// N00B_LOC_STRING — source location (unused, returns "").
#define N00B_LOC_STRING() ""
