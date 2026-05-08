#ifndef N00B_TEST_PORTABILITY_H
#define N00B_TEST_PORTABILITY_H

#if defined(_WIN32)

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static int
n00b_test_asprintf(char **out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);

    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);

    if (needed < 0) {
        va_end(ap);
        *out = NULL;
        return -1;
    }

    char *buf = malloc((size_t)needed + 1);

    if (!buf) {
        va_end(ap);
        *out = NULL;
        return -1;
    }

    int written = vsnprintf(buf, (size_t)needed + 1, fmt, ap);
    va_end(ap);

    if (written < 0) {
        free(buf);
        *out = NULL;
        return -1;
    }

    *out = buf;
    return written;
}

#define asprintf n00b_test_asprintf

#endif

#endif
