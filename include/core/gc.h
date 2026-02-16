#pragma once

extern void
_n00b_add_gc_root(void *ptr, char *loc) _kargs
{
    uint32_t num_words = 1;
};

extern bool n00b_remove_gc_root(void *ptr);

#define n00b_add_gc_root(ptr, ...)                                                             \
    _n00b_add_gc_root(ptr, N00B_LOC_STRING() __VA_OPT__(, __VA_ARGS__))

#define n00b_gc_register_root(ptr, n)                                                          \
    _n00b_add_gc_root(ptr, N00B_LOC_STRING(), .num_words = (uint32_t)(n))
