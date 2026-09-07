#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "ir/diag.h"
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

static inline core_string_t
slurp(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f) {
        printf("FAIL  cannot open %s\n", path);
        fails++;

        return (core_string_t){0};
    }

    fseek(f, 0, SEEK_END);

    long n = ftell(f);

    rewind(f);

    char  *p = core_alloc_size(1, (size_t)n + 1);
    size_t r = fread(p, 1, (size_t)n, f);

    fclose(f);

    core_string_t s = core_string_from_raw(p, (int64_t)r);

    core_free(p);

    return s;
}

static inline void
dump_produced_codes(const char *leaf, const bool *produced)
{
    const char *dir = getenv("IR_CODE_DUMP");

    if (!dir) {
        return;
    }

    char path[1024];

    snprintf(path, sizeof path, "%s/%s", dir, leaf);

    FILE *f = fopen(path, "w");

    if (!f) {
        check("the produced-code list could be written", false);

        return;
    }

    for (uint16_t i = 0; i < IR_DIAG_COUNT; i++) {
        if (produced[i]) {
            fprintf(f, "%s\n", ir_diag_code((ir_diagcode_t)i));
        }
    }

    fclose(f);
}

static inline int
report(const char *suite)
{
    if (!fails) {
        printf("%s: all checks passed\n", suite);
    }

    return fails ? 1 : 0;
}
