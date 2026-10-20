#pragma once

#define _CORE_CAT2(a, b) a ## b
#define CORE_CONCAT(a, b) _CORE_CAT2(a, b)

#define _CORE_FIRST_IMPL(x, ...) x
#define CORE_FIRST(...) _CORE_FIRST_IMPL(__VA_ARGS__)

#define typeid(...) _TYPEID_SEL(__VA_ARGS__, _TYPEID4, _TYPEID3, _TYPEID2, _TYPEID1)(__VA_ARGS__)
#define _TYPEID_SEL(_1,_2,_3,_4,N,...) N
#define _TYPEID1(a) a
#define _TYPEID2(a, b) CORE_CONCAT(a ## _, b)
#define _TYPEID3(a, b, c) CORE_CONCAT(CORE_CONCAT(a ## _, b), _ ## c)
#define _TYPEID4(a, b, c, d) CORE_CONCAT(CORE_CONCAT(CORE_CONCAT(a ## _, b), _ ## c), _ ## d)

#define typehash(...) ((uint64_t)0)

#define CORE_LOC_STRING() ""
