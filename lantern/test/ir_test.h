#pragma once

#include <stdbool.h>
#include <stdio.h>

#include "lib/alloc.h"
#include "lib/string.h"

static int fails = 0;

static inline void
check(const char *what, bool ok)
{
    if (!ok) {
        fails++;
        printf("FAIL  %s\n", what);
    }
}

static inline ncc_string_t
slurp(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f) {
        printf("FAIL  cannot open %s\n", path);
        fails++;

        return (ncc_string_t){0};
    }

    fseek(f, 0, SEEK_END);

    long n = ftell(f);

    rewind(f);

    char  *p = ncc_alloc_size(1, (size_t)n + 1);
    size_t r = fread(p, 1, (size_t)n, f);

    fclose(f);

    ncc_string_t s = ncc_string_from_raw(p, (int64_t)r);

    ncc_free(p);

    return s;
}

static inline int
report(const char *suite)
{
    if (!fails) {
        printf("%s: all checks passed\n", suite);
    }

    return fails ? 1 : 0;
}
