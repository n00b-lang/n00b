#pragma once

#include <stdint.h>

enum { VK_INT = 1, VK_REAL = 2, VK_TEXT = 3, VK_NULL = 5 };

typedef union {
    int64_t i;
    double d;
    void *p;
} Raw;

typedef struct {
    int kind;
    union {
        int64_t i;
        double d;
        const char *s;
    } v;
} Value;

typedef struct {
    Raw raw;
    uint8_t flag;
} Cell;

int value_kind(const Value *value);
uint8_t cell_flag(const Cell *cell, const Raw *raw);
